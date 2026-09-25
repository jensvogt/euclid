// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <algorithm>
#include <map>

// Euclid includes
#include <EamServer.h>
#include <euclid/core/HttpUtils.h>
#include <euclid/core/OidcClient.h>
#include <euclid/core/SamlProvider.h>
#include <euclid/core/Scheduler.h>
#include <euclid/core/monitoring/MetricEventBus.h>
#include <euclid/core/monitoring/MonitoringTimer.h>
#include <euclid/dto/eam/ChangeNamespaceRequest.h>
#include <euclid/dto/eam/ChangePasswordRequest.h>
#include <euclid/dto/eam/ChangeUserIdRequest.h>
#include <euclid/dto/eam/CreateAccountRequest.h>
#include <euclid/dto/eam/CreateAccountResponse.h>
#include <euclid/dto/eam/CreateNamespaceRequest.h>
#include <euclid/dto/eam/CreateNamespaceResponse.h>
#include <euclid/dto/eam/DeleteAccountRequest.h>
#include <euclid/dto/eam/DeleteNamespaceRequest.h>
#include <euclid/dto/eam/DeleteUserGroupRequest.h>
#include <euclid/dto/eam/GetAccountRequest.h>
#include <euclid/dto/eam/GetAccountResponse.h>
#include <euclid/dto/eam/ListAccountsRequest.h>
#include <euclid/dto/eam/ListAccountsResponse.h>
#include <euclid/dto/eam/ListNamespacesRequest.h>
#include <euclid/dto/eam/ListNamespacesResponse.h>
#include <euclid/dto/eam/ListUserGroupsRequest.h>
#include <euclid/dto/eam/ListUserGroupsResponse.h>
#include <euclid/dto/eam/OidcAuthorizeRequest.h>
#include <euclid/dto/eam/OidcAuthorizeResponse.h>
#include <euclid/dto/eam/OidcLoginRequest.h>
#include <euclid/dto/eam/SamlAuthorizeResponse.h>
#include <euclid/dto/eam/UserGroupAddUserRequest.h>

namespace Euclid::EAM {

    namespace beast = boost::beast;
    namespace http = beast::http;

    // ── Helpers ──────────────────────────────────────────────────────────────

    namespace {
        // Looks up the caller identity resolved by EamServer::Authenticate(), by user ID.
        // Distinguishing an expired token lets handlers return a more specific error than a plain 401.
        struct AuthResult {
            std::optional<Database::Entity::EAM::User> user;
            bool tokenExpired{false};
            std::string denialReason;
        };
    }// namespace

    static AuthResult authenticate(const request<string_body> &req) {
        const auto auth = EamServer::Authenticate(req);
        if (!auth.subject.has_value()) {
            return {.user = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason};
        }
        return {.user = Database::RepositoryFactory::instance().eamRepository()->findUserByUserId(*auth.subject)};
    }

    static response<string_body> unauthorized(const request<string_body> &req, const AuthResult &auth) {
        return EamServer::Unauthorized(req, {.subject = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason});
    }

    // Whether user is a global administrator - derived from membership in the "administrator"
    // user group (Database::kEamAdministratorGroupName) rather than a per-user flag, so admin
    // status is granted/revoked the same way any other group membership is.
    static bool isAdmin(const Database::Entity::EAM::User &user) {
        return Database::IsEamAdmin(*Database::RepositoryFactory::instance().eamRepository(), user.userId);
    }

    // Whether user may administer accountId's namespaces: either an installation administrator, or
    // holding the account-administrator role in that account. Account creation and deletion stay
    // installation-admin-only (see handleCreateAccount/handleDeleteAccount) since those are
    // platform-level operations, not delegable to an account owner.
    static bool isAccountAdmin(const Database::Entity::EAM::User &user, const std::string &accountId) {

        if (isAdmin(user)) return true;

        const auto role = std::string(Core::BuiltinRoles::AccountAdministrator);
        const auto repo = Database::RepositoryFactory::instance().eamRepository();

        return std::ranges::any_of(repo->findGrantsByPrincipals(Database::PrincipalsOf(user)),
                                   [&](const auto &grant) { return grant.role == role && grant.accountId == accountId; });
    }

    // Token lifetime, and the matching lifetime given to the Session record created on login -
    // named so the two can't silently drift apart.
    constexpr auto kSessionTtl = std::chrono::hours(1);

    // Timer/counter names shared by every handler below - one series per action, labeled
    // "method"=<action>.
    constexpr auto kServiceTimer = "eam-service-time";
    constexpr auto kServiceCounter = "eam-service-count";

    // Counts users with at least one non-expired session. Fired as a gauge (see
    // reportCurrentUsers()) rather than derived per-request, since it's shared state - every
    // access instance in an autoscaled pool would otherwise report/poll it independently.
    static long countCurrentUsers() {
        const auto repo = Database::RepositoryFactory::instance().eamRepository();
        const auto users = repo->listUsers("", 0, 0, "");
        const auto now = Core::DateTimeUtils::UtcDateTimeNow();

        long currentUsers = 0;
        for (const auto &user: users) {
            const bool hasActiveSession = std::ranges::any_of(user.sessions, [&](const auto &session) {
                return Core::DateTimeUtils::FromISO8601(session.expiresAt) > now;
            });
            if (hasActiveSession) ++currentUsers;
        }
        return currentUsers;
    }

    static void reportCurrentUsers() {
        Core::Monitoring::MetricEventBus::instance().sigMetricGauge("eam-current-users", "", "", static_cast<double>(countCurrentUsers()));
    }

    // Everything a login does once it has settled who the caller is: the access key, the session
    // record and the token.
    //
    // Shared by the password path and the OIDC one (see handleOidcLogin) deliberately - the point
    // of federating authentication is that only the *proof* differs. A session that came in
    // through OneLogin is the same session, with the same key and the same one-hour token, and
    // nothing downstream of here can tell the two apart or has to care.
    static Dto::EAM::LoginResponse issueSession(const Database::Entity::EAM::User &user) {

        const auto repo = Database::RepositoryFactory::instance().eamRepository();

        auto updatedUser = user;

        // Reuses the caller's existing active access key if there is one, so login stays
        // self-sufficient for SigV4-signed calls without minting a new key (and invalidating
        // local credentials elsewhere) on every login.
        const Database::Entity::EAM::AccessKey *existingKey = nullptr;
        for (const auto &candidate: updatedUser.accessKeys) {
            if (candidate.active) {
                existingKey = &candidate;
                break;
            }
        }

        Database::Entity::EAM::AccessKey key;
        if (existingKey != nullptr) {
            key = *existingKey;
        } else {
            key.accessKeyId = Core::CryptoUtils::GenerateAccessKeyId();
            key.secretAccessKey = Core::CryptoUtils::GenerateSecretAccessKey();
            key.active = true;
            key.created = Core::DateTimeUtils::ToISO8601(Core::DateTimeUtils::UtcDateTimeNow());
            updatedUser.accessKeys.push_back(key);
            log_info << "Access key provisioned on login, userId: " << user.userId << ", accessKeyId: " << key.accessKeyId;
        }

        const auto nowIso = Core::DateTimeUtils::ToISO8601(Core::DateTimeUtils::UtcDateTimeNow());

        // Dropped before the new one is added, because nothing ever removed them and the cost is
        // not paid here - it is paid by every authenticated request in the installation. Both the
        // access-key lookup and the grant lookup fetch this whole document, twice per request
        // (once at the gateway, once in the module), so every stored session rides along with each
        // of them. An admin account here had reached 5664 sessions and 522 KB, and a lookup that
        // takes 0.4 ms against an ordinary user was taking 4.8 ms against that one.
        //
        // Compared as text rather than parsed: both sides come from ToISO8601, whose fixed
        // "%FT%TZ" layout orders lexicographically exactly as it orders in time, and which avoids
        // FromISO8601's local-time round trip. An entry carrying no expiry sorts first and is
        // removed, which is what should happen to one written before sessions had an expiry.
        std::erase_if(updatedUser.sessions, [&nowIso](const Database::Entity::EAM::Session &existing) {
            return existing.expiresAt < nowIso;
        });

        Database::Entity::EAM::Session session;
        session.createdAt = nowIso;
        session.expiresAt = Core::DateTimeUtils::ToISO8601(Core::DateTimeUtils::UtcDateTimeNow() + kSessionTtl);
        updatedUser.sessions.push_back(session);

        // Always persisted (not just when a new access key was minted) - otherwise a login that
        // reuses an existing key would never record its session, and "current users" would only
        // ever count first-time logins.
        repo->upsertUser(updatedUser);

        Dto::EAM::LoginResponse response;
        response.token = Core::JwtUtils::CreateToken(user.userId, EamServer::JwtSecret(), kSessionTtl);
        response.user = user.userId;
        response.accountId = user.accountId;
        response.region = user.region;
        response.accessKeyId = key.accessKeyId;
        response.secretAccessKey = key.secretAccessKey;
        response.createdAt = key.created;
        response.isAdmin = isAdmin(user);
        return response;
    }

    // Looks up the user by userId, falling back to email, and checks the password
    // against the stored PBKDF2-HMAC-SHA256 hash.
    static Dto::EAM::LoginResponse doLogin(const Dto::EAM::LoginRequest &request) {

        const auto repo = Database::RepositoryFactory::instance().eamRepository();

        const std::optional<Database::Entity::EAM::User> user = !request.userId.empty()
                                                                    ? repo->findUserByUserId(request.userId)
                                                                    : repo->findUserByEmail(request.email);

        if (!user.has_value() || !Core::PasswordUtils::Verify(request.password, user->password)) {
            log_warning << "Login failed, userId: " << request.userId << ", email: " << request.email;
            return {};
        }

        // A technical principal is an application's identity, not a person's: it has an access key
        // to sign with and deliberately no way in through here. A federated user is refused by the
        // same flag, for the same reason - it has no password, and an empty one must not become a
        // way in. Checked after the password so that a caller cannot tell the kinds of account
        // apart by how quickly they are refused.
        if (!user->loginEnabled) {
            log_warning << "Login refused for a user that does not log in with a password, userId: " << user->userId;
            return {};
        }

        log_info << "Login succeeded, userId: " << user->userId;
        return issueSession(*user);
    }

    // Hands back a session with a fresh expiry, for a caller that already has one.
    //
    // A session lasts an hour and nothing about that is negotiable, so a client that sits open for
    // longer has to renew: without this the only way back is to ask the operator for their password
    // again, which is a poor answer for an application somebody left running over lunch.
    //
    // Deliberately not a way *in*. It authenticates like every other action - a still-valid token
    // or a signed request - so it extends a session rather than creating one, and an expired token
    // is refused here exactly as it is everywhere else. Renewing is therefore something a client
    // does before its token runs out, not after.
    static response<string_body> handleRefreshSession(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "refresh-session");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        log_debug << "Session refreshed, userId: " << auth.user->userId;

        // The same answer login gives, so a client has one shape to handle and can adopt whatever
        // it needs from it - including the access key, which issueSession() reuses rather than
        // reissuing.
        return EamServer::JsonResponse(req, status::ok, issueSession(*auth.user).toJson());
    }

    static response<string_body> handleLogin(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "login");

        boost::json::value jv;
        if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EAM::LoginRequest>(jv);
        const auto response = doLogin(request);

        if (response.token.empty()) {
            return EamServer::ErrorResponse(req, status::unauthorized, "Invalid credentials");
        }

        return EamServer::JsonResponse(req, status::ok, response.toJson());
    }

    // ── OIDC login ───────────────────────────────────────────────────────────

    namespace {

        // 302 rather than a body: both browser-facing endpoints exist to move a browser along, and
        // the only thing either has to say is where to next.
        response<string_body> redirect(const request<string_body> &req, const std::string &location) {
            response<string_body> res{status::found, req.version()};
            res.set(field::location, location);
            res.set(field::cache_control, "no-store");
            res.keep_alive(req.keep_alive());
            res.prepare_payload();
            return res;
        }

        Core::OidcClient oidcClient(const Core::OidcConfiguration &config) {
            return {config, EamServer::JwtSecret()};
        }

        // What an installation is willing to do with somebody its identity provider has vouched for.
        // The two federations answer this from their own configuration blocks, and the answer means
        // the same thing in both.
        struct ProvisioningPolicy {
            bool jitProvisioning{true};
            bool linkExistingUsers{false};
            std::string accountId;
        };
    }// namespace

    // Finds the euclid user a verified identity belongs to, creating one if the installation is
    // configured to and there is nothing to find.
    //
    // Matching is on the provider's subject, never on the name or the email address: those are
    // editable in the provider, and matching on them would mean a renamed person silently gets a
    // second account - or, worse, that whoever inherits a recycled email address inherits the
    // account that went with it.
    //
    // Shared by OIDC and SAML deliberately. This is where an installation's rules about who may
    // have an account live, and two copies of them would be two chances to have different rules by
    // accident.
    static std::optional<Database::Entity::EAM::User> resolveFederatedUser(const Core::FederatedIdentity &identity,
                                                                          const ProvisioningPolicy &policy,
                                                                          std::string &refusal) {

        const auto repo = Database::RepositoryFactory::instance().eamRepository();

        if (auto existing = repo->findUserByFederatedSubject(identity.provider, identity.subject); existing.has_value()) {

            // The email is the one thing kept in step with the provider: it is what an operator
            // recognises a user by, and it is not what anything is keyed on, so letting it drift
            // costs nothing and following it breaks nothing.
            if (!identity.email.empty() && existing->email != identity.email) {
                existing->email = identity.email;
                existing->modified = std::chrono::system_clock::now();
                repo->upsertUser(*existing);
            }
            return existing;
        }

        if (auto sameName = repo->findUserByUserId(identity.userId); sameName.has_value()) {

            if (!policy.linkExistingUsers) {
                refusal = "A euclid user '" + identity.userId + "' already exists and is not linked to this identity provider";
                log_warning << "Federated login refused, a local user of that name already exists, provider: " << identity.provider
                            << ", userId: " << identity.userId << ", subject: " << identity.subject
                            << " - set link-existing-users to adopt it";
                return std::nullopt;
            }

            log_info << "Federated login adopting an existing user, provider: " << identity.provider
                     << ", userId: " << sameName->userId << ", subject: " << identity.subject;
            sameName->federatedProvider = identity.provider;
            sameName->federatedSubject = identity.subject;
            if (!identity.email.empty()) sameName->email = identity.email;
            sameName->modified = std::chrono::system_clock::now();
            repo->upsertUser(*sameName);
            return sameName;
        }

        if (!policy.jitProvisioning) {
            refusal = "No euclid user for this identity, and jit-provisioning is off";
            log_warning << "Federated login refused, no user and no provisioning, provider: " << identity.provider
                        << ", userId: " << identity.userId << ", subject: " << identity.subject;
            return std::nullopt;
        }

        // Created with nothing granted: the provider says who this is, and that is all it says.
        // What the person may do here is euclid's own question, answered by an administrator with
        // grant-namespace-access - so a provider that adds a person to a directory does not
        // thereby give them anything in this installation.
        Database::Entity::EAM::User user;
        user.userId = identity.userId;
        user.ern = Core::createEamUserErn(policy.accountId, identity.userId);
        user.email = identity.email;
        user.accountId = policy.accountId;
        user.region = Core::Configuration::instance().getOr<std::string>("euclid.region", "eu-central-1");
        user.federatedProvider = identity.provider;
        user.federatedSubject = identity.subject;

        // No password, and no way to reach the password path with the empty hash that leaves
        // behind - the same flag that keeps an application's technical principal out of "eam
        // login" (see doLogin).
        user.password = {};
        user.loginEnabled = false;
        user.created = user.modified = std::chrono::system_clock::now();

        const auto created = repo->upsertUser(user);
        log_info << "User provisioned from a federated login, provider: " << identity.provider
                 << ", userId: " << created.userId << ", subject: " << identity.subject
                 << ", accountId: " << created.accountId;
        return created;
    }

    // Refuses an assertion that has been presented before, and records the one being accepted.
    //
    // Kept on the user rather than in memory because eam is a pool: an assertion the instance that
    // first saw it would refuse is otherwise simply presented to another one. Written before the
    // session is issued, so a replay arriving while the first login is still in flight loses the
    // race rather than winning it.
    static bool acceptAssertionOnce(Database::Entity::EAM::User &user, const Core::FederatedIdentity &identity) {

        if (identity.assertionId.empty()) return true;

        const auto nowIso = Core::DateTimeUtils::ToISO8601(Core::DateTimeUtils::UtcDateTimeNow());
        if (std::ranges::any_of(user.seenAssertions, [&](const auto &seen) { return seen.assertionId == identity.assertionId; })) {
            return false;
        }

        // Pruned as they are added, the same way sessions are: an expired assertion is refused on
        // its own expiry, so remembering its ID any longer costs a read on every request that
        // fetches this document and buys nothing.
        std::erase_if(user.seenAssertions, [&nowIso](const auto &seen) { return seen.expiresAt < nowIso; });
        user.seenAssertions.push_back({.assertionId = identity.assertionId,
                                       .expiresAt = Core::DateTimeUtils::ToISO8601(identity.assertionExpiresAt)});
        return true;
    }

    // Reads the OIDC configuration, or answers with why there is nothing to read.
    static std::optional<Core::OidcConfiguration> oidcConfiguration(const request<string_body> &req, std::optional<response<string_body> > &refusal) {

        auto config = Core::OidcConfiguration::FromConfiguration("eam");
        if (!config.enabled) {
            refusal = EamServer::ErrorResponse(req, status::not_found, "OIDC login is not enabled");
            return std::nullopt;
        }
        if (const auto problems = config.Validate(); !problems.empty()) {
            std::string message;
            for (const auto &problem: problems) {
                if (!message.empty()) message += "; ";
                message += problem;
            }
            log_error << "OIDC login is enabled but not usable: " << message;
            refusal = EamServer::ErrorResponse(req, status::internal_server_error, "OIDC login is misconfigured: " + message);
            return std::nullopt;
        }
        return config;
    }

    // Starts a login: answers with where to send the person to authenticate.
    //
    // Both shapes of caller come through here. One posts JSON and gets the URL back to open
    // itself, which is what the CLI does with its loopback listener; the other is a browser that
    // was sent to this endpoint and is redirected onward, which is what a web front end links to.
    static response<string_body> handleOidcAuthorize(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "oidc-authorize");

        std::optional<response<string_body> > refusal;
        const auto config = oidcConfiguration(req, refusal);
        if (!config.has_value()) return *refusal;

        const bool browser = req.method() == verb::get;

        Dto::EAM::OidcAuthorizeRequest request;
        if (browser) {
            const auto parameters = Core::ParseQueryParameters(req.target());
            if (const auto it = parameters.find("redirect-uri"); it != parameters.end()) request.redirectUri = it->second;
            if (const auto it = parameters.find("return-to"); it != parameters.end()) request.returnTo = it->second;
        } else {
            boost::json::value jv;
            if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;
            request = boost::json::value_to<Dto::EAM::OidcAuthorizeRequest>(jv);
        }

        // Refused before the flow starts, so an unacceptable destination never reaches the sealed
        // state. The token travels in the fragment of this redirect, so where it may point is not
        // the caller's decision to make - see OidcConfiguration::returnToPrefixes.
        if (!config->IsReturnToAllowed(request.returnTo)) {
            log_warning << "OIDC authorization refused, return-to is not permitted: " << request.returnTo;
            return EamServer::ErrorResponse(req, status::bad_request,
                                            "return-to is not one of the permitted destinations (oidc.return-to-prefixes)");
        }

        try {
            const auto authorization = oidcClient(*config).BeginAuthorization(request.redirectUri, request.returnTo);
            log_info << "OIDC authorization started, redirectUri: " << (request.redirectUri.empty() ? config->redirectUri : request.redirectUri);

            if (browser) return redirect(req, authorization.url);

            Dto::EAM::OidcAuthorizeResponse response;
            response.authorizationUrl = authorization.url;
            response.state = authorization.state;
            return EamServer::JsonResponse(req, status::ok, response.toJson());

        } catch (const Core::OidcError &e) {
            log_error << "OIDC authorization could not be started, error: " << e.what();
            return EamServer::ErrorResponse(req, status::bad_gateway, std::string("OIDC provider unusable: ") + e.what());
        }
    }

    // Finishes a login: redeems the code, verifies who came back, and issues an ordinary euclid
    // session for them.
    static response<string_body> handleOidcLogin(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "oidc-login");

        std::optional<response<string_body> > refusal;
        const auto config = oidcConfiguration(req, refusal);
        if (!config.has_value()) return *refusal;

        const bool browser = req.method() == verb::get;

        Dto::EAM::OidcLoginRequest request;
        if (browser) {
            const auto parameters = Core::ParseQueryParameters(req.target());

            // A provider reports a refusal by redirecting back with an error instead of a code -
            // a person who cancelled at the login screen, or an application they may not use.
            if (const auto error = parameters.find("error"); error != parameters.end()) {
                const auto description = parameters.find("error_description");
                log_warning << "OIDC callback carried an error, error: " << error->second;
                return EamServer::ErrorResponse(req, status::unauthorized,
                                                "Identity provider refused the login: " + error->second +
                                                        (description == parameters.end() ? "" : " (" + description->second + ")"));
            }
            if (const auto it = parameters.find("code"); it != parameters.end()) request.code = it->second;
            if (const auto it = parameters.find("state"); it != parameters.end()) request.state = it->second;
        } else {
            boost::json::value jv;
            if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;
            request = boost::json::value_to<Dto::EAM::OidcLoginRequest>(jv);
        }

        Core::FederatedIdentity identity;
        Core::OidcState state;
        try {
            identity = oidcClient(*config).Complete(request.code, request.state, state);
        } catch (const Core::OidcError &e) {
            log_warning << "OIDC login failed, error: " << e.what();
            return EamServer::ErrorResponse(req, status::unauthorized, std::string("OIDC login failed: ") + e.what());
        }

        std::string userRefusal;
        const auto user = resolveFederatedUser(identity,
                                               {.jitProvisioning = config->jitProvisioning,
                                                .linkExistingUsers = config->linkExistingUsers,
                                                .accountId = config->accountId},
                                               userRefusal);
        if (!user.has_value()) {
            return EamServer::ErrorResponse(req, status::forbidden, userRefusal);
        }

        const auto response = issueSession(*user);
        log_info << "OIDC login succeeded, userId: " << user->userId << ", issuer: " << identity.issuer;

        // Handed over in the fragment, not the query, when a browser is being sent onward: a
        // fragment is not sent to the server it points at, does not reach an access log and is not
        // passed on in a Referer header. The front end reads it out of location.hash and should
        // clear it once it has.
        // Checked again on the way out, not because the state could have been altered - it is
        // sealed - but because the configuration may have been narrowed since the flow started,
        // and the narrowing should take effect for logins already in the air.
        if (browser && !state.returnTo.empty() && config->IsReturnToAllowed(state.returnTo)) {
            const auto separator = state.returnTo.find('#') == std::string::npos ? "#" : "&";
            return redirect(req, state.returnTo + separator + "token=" + response.token + "&accessKeyId=" + response.accessKeyId);
        }

        return EamServer::JsonResponse(req, status::ok, response.toJson());
    }

    // ── SAML login ───────────────────────────────────────────────────────────

    namespace {

        Core::SamlProvider samlProvider(const Core::SamlConfiguration &config) {
            return {config, EamServer::JwtSecret()};
        }

        // Hands the session to a listener the person signing in is already running - the CLI's.
        //
        // A form that submits itself, rather than a redirect: a redirect would have to carry the
        // token in the URL, where it would sit in a history entry and in whatever the listener
        // logs. This posts it in a body to a port on the same machine, and the browser is left on
        // a page that says what happened.
        response<string_body> handoffPage(const request<string_body> &req, const std::string &target, const std::string &body) {

            const std::string html =
                    "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><title>euclid</title></head>"
                    "<body style=\"font-family:sans-serif;padding:3rem\" onload=\"document.forms[0].submit()\">"
                    "<h1>Signed in</h1><p>You can close this tab and go back to the terminal.</p>"
                    "<form method=\"post\" action=\"" + target + "\">"
                    "<input type=\"hidden\" name=\"session\" value=\"" + Core::CryptoUtils::Base64UrlEncode(body) + "\">"
                    "<noscript><button type=\"submit\">Continue</button></noscript>"
                    "</form></body></html>";

            response<string_body> res{status::ok, req.version()};
            res.set(field::content_type, "text/html; charset=utf-8");
            res.set(field::cache_control, "no-store");
            res.keep_alive(req.keep_alive());
            res.body() = html;
            res.prepare_payload();
            return res;
        }

    }// namespace

    // Reads the SAML configuration, or answers with why there is nothing to read.
    //
    // What "usable" means depends on what is being asked for: starting a login needs somewhere to
    // send the browser, consuming an assertion does not. An installation whose people get their
    // assertions from their provider's API only ever does the second.
    static std::optional<Core::SamlConfiguration> samlConfiguration(const request<string_body> &req, std::optional<response<string_body> > &refusal,
                                                                    const bool forAuthentication = false) {

        auto config = Core::SamlConfiguration::FromConfiguration("eam");
        if (!config.enabled) {
            refusal = EamServer::ErrorResponse(req, status::not_found, "SAML login is not enabled");
            return std::nullopt;
        }
        if (const auto problems = forAuthentication ? config.ValidateForAuthentication() : config.Validate(); !problems.empty()) {
            std::string message;
            for (const auto &problem: problems) {
                if (!message.empty()) message += "; ";
                message += problem;
            }
            log_error << "SAML login is enabled but not usable: " << message;
            refusal = EamServer::ErrorResponse(req, status::internal_server_error, "SAML login is misconfigured: " + message);
            return std::nullopt;
        }
        return config;
    }

    // This installation's SP metadata, which is what an identity provider administrator is asked
    // for when registering euclid. Public, and deliberately so: it states an entity ID and a URL,
    // both of which the provider is about to be told anyway.
    static response<string_body> handleSamlMetadata(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "saml-metadata");

        std::optional<response<string_body> > refusal;
        const auto config = samlConfiguration(req, refusal);
        if (!config.has_value()) return *refusal;

        response<string_body> res{status::ok, req.version()};
        res.set(field::content_type, "application/samlmetadata+xml");
        res.keep_alive(req.keep_alive());
        res.body() = samlProvider(*config).Metadata();
        res.prepare_payload();
        return res;
    }

    // Starts a login: answers with where to send the person to authenticate. Same two shapes of
    // caller as the OIDC side - a browser that is redirected onward, or a program that is told the
    // URL to open.
    static response<string_body> handleSamlAuthorize(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "saml-authorize");

        std::optional<response<string_body> > refusal;
        const auto config = samlConfiguration(req, refusal, true);
        if (!config.has_value()) return *refusal;

        const bool browser = req.method() == verb::get;

        std::string returnTo;
        if (browser) {
            const auto parameters = Core::ParseQueryParameters(req.target());
            if (const auto it = parameters.find("return-to"); it != parameters.end()) returnTo = it->second;
        } else {
            boost::json::value jv;
            if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;
            returnTo = Core::GetStringValue(jv, "returnTo");
        }

        if (!config->IsReturnToAllowed(returnTo)) {
            log_warning << "SAML authentication refused, return-to is not permitted: " << returnTo;
            return EamServer::ErrorResponse(req, status::bad_request,
                                            "return-to is not one of the permitted destinations (saml.return-to-prefixes)");
        }

        try {
            const auto authentication = samlProvider(*config).BeginAuthentication(returnTo);
            log_info << "SAML authentication started, requestId: " << authentication.requestId;

            if (browser) return redirect(req, authentication.url);

            Dto::EAM::SamlAuthorizeResponse response;
            response.authenticationUrl = authentication.url;
            response.relayState = authentication.relayState;
            return EamServer::JsonResponse(req, status::ok, response.toJson());

        } catch (const Core::SamlError &e) {
            log_error << "SAML authentication could not be started, error: " << e.what();
            return EamServer::ErrorResponse(req, status::internal_server_error, std::string("SAML request could not be built: ") + e.what());
        }
    }

    // The assertion consumer service: what the identity provider posts the signed assertion to.
    //
    // Arrives as an ordinary HTML form post from a browser, which is what the HTTP-POST binding
    // is, so the two fields are read from the body rather than from JSON.
    static response<string_body> handleSamlAcs(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "saml-acs");

        std::optional<response<string_body> > refusal;
        const auto config = samlConfiguration(req, refusal);
        if (!config.has_value()) return *refusal;

        // A form body is a query string that happens to travel in a body, so it is read the same
        // way - and a caller that would rather send JSON (the CLI, a front end doing its own
        // posting) may do that instead.
        std::string samlResponse, relayState;
        if (const auto contentType = std::string(req[field::content_type]); contentType.starts_with("application/json")) {
            boost::json::value jv;
            if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;
            samlResponse = Core::GetStringValue(jv, "samlResponse");
            relayState = Core::GetStringValue(jv, "relayState");
        } else {
            const auto fields = Core::ParseQueryParameters("?" + req.body());
            if (const auto it = fields.find("SAMLResponse"); it != fields.end()) samlResponse = it->second;
            if (const auto it = fields.find("RelayState"); it != fields.end()) relayState = it->second;
        }

        Core::FederatedIdentity identity;
        std::string returnTo;
        try {
            identity = samlProvider(*config).Consume(samlResponse, relayState, returnTo);
        } catch (const Core::SamlError &e) {
            log_warning << "SAML login failed, error: " << e.what();
            return EamServer::ErrorResponse(req, status::unauthorized, std::string("SAML login failed: ") + e.what());
        }

        std::string userRefusal;
        auto user = resolveFederatedUser(identity,
                                         {.jitProvisioning = config->jitProvisioning,
                                          .linkExistingUsers = config->linkExistingUsers,
                                          .accountId = config->accountId},
                                         userRefusal);
        if (!user.has_value()) {
            return EamServer::ErrorResponse(req, status::forbidden, userRefusal);
        }

        // Replay is checked against what the provider signed, so it is checked after the signature
        // and before anything is issued.
        if (!acceptAssertionOnce(*user, identity)) {
            log_warning << "SAML login refused, assertion already used, userId: " << user->userId << ", assertionId: " << identity.assertionId;
            return EamServer::ErrorResponse(req, status::unauthorized, "SAML login failed: this assertion has already been used");
        }
        Database::RepositoryFactory::instance().eamRepository()->upsertUser(*user);

        const auto response = issueSession(*user);
        log_info << "SAML login succeeded, userId: " << user->userId << ", issuer: " << identity.issuer;

        if (!returnTo.empty() && config->IsReturnToAllowed(returnTo)) {

            // A loopback destination is the CLI waiting on its own port, and it gets the session in
            // a form post rather than in a URL. Anywhere else is a front end, which gets it in the
            // fragment - not sent to any server, not in an access log, not in a Referer.
            if (Core::IsLoopbackUrl(returnTo)) return handoffPage(req, returnTo, response.toJson());

            const auto separator = returnTo.find('#') == std::string::npos ? "#" : "&";
            return redirect(req, returnTo + separator + "token=" + response.token + "&accessKeyId=" + response.accessKeyId);
        }

        return EamServer::JsonResponse(req, status::ok, response.toJson());
    }

    // Registers a new user. Requires the caller to be an administrator, with one exception:
    // when the user store is completely empty, the request is allowed through unauthenticated
    // and the new user is force-promoted to administrator, so there's always a way to bootstrap
    // the first account.
    static response<string_body> handleRegister(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "register");

        const auto repo = Database::RepositoryFactory::instance().eamRepository();
        const bool bootstrap = repo->countUsers() <= 0;

        if (!bootstrap) {
            const auto auth = authenticate(req);
            if (!auth.user.has_value()) {
                return unauthorized(req, auth);
            }
            if (!isAdmin(*auth.user)) {
                return EamServer::ErrorResponse(req, status::forbidden, "Administrator privileges required");
            }
        }

        boost::json::value jv;
        if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EAM::RegisterRequest>(jv);
        if (request.userId.empty() || request.password.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request, "userId and password are required");
        }

        if (repo->userExists(request.userId)) {
            return EamServer::ErrorResponse(req, status::conflict, "User already exists");
        }

        Database::Entity::EAM::User user;
        user.userId = request.userId;
        user.ern = Core::createEamUserErn(request.accountId, request.userId);
        user.email = request.email;
        user.accountId = request.accountId;
        user.region = request.region;
        user.password = Core::PasswordUtils::Hash(request.password);
        user.created = std::chrono::system_clock::now();
        user.modified = std::chrono::system_clock::now();

        const auto saved = repo->upsertUser(user);

        // Administrator status is membership in the "administrator" group, not a per-user flag -
        // add the new user to it (creating the group on first use) when bootstrapping the very
        // first account, or when an existing administrator asked for one via request.isAdmin.
        const bool grantAdmin = bootstrap || request.isAdmin;
        if (grantAdmin) {
            auto group = repo->findUserGroupByName(Database::kEamAdministratorGroupName);
            Database::Entity::EAM::UserGroup adminGroup = group.value_or(Database::Entity::EAM::UserGroup{});
            if (!group.has_value()) {
                adminGroup.name = Database::kEamAdministratorGroupName;
                adminGroup.accountId = saved.accountId;
                adminGroup.region = saved.region;
                adminGroup.ern = Core::createEamUserGroupErn(saved.accountId, Database::kEamAdministratorGroupName);
                adminGroup.description = "Euclid administrators group";
                adminGroup.created = std::chrono::system_clock::now();
            }
            if (!std::ranges::contains(adminGroup.userIds, saved.userId)) {
                adminGroup.userIds.push_back(saved.userId);
            }
            adminGroup.modified = std::chrono::system_clock::now();
            repo->upsertUserGroup(adminGroup);
        }

        log_info << "User registered, userId: " << saved.userId << ", admin: " << std::boolalpha << grantAdmin << (bootstrap ? " (bootstrap)" : "");

        Dto::EAM::RegisterResponse response;
        response.user.userId = saved.userId;
        response.user.ern = saved.ern;
        response.user.email = saved.email;
        response.user.accountId = saved.accountId;
        response.user.region = saved.region;
        return EamServer::JsonResponse(req, status::created, response.toJson());
    }

    // Lists users. Requires the caller to be an authenticated administrator - unlike
    // handleRegister() there's no bootstrap exception, since an empty store has nothing to list.
    // Reads one user. Administrator-only, like every other way of looking at who exists: a user
    // list is the shape of an organisation, and an ordinary user has no business reading it.
    static response<string_body> handleGetUser(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-user");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);
        if (!isAdmin(*auth.user)) {
            return EamServer::ErrorResponse(req, status::forbidden, "Administrator privileges required");
        }

        boost::json::value jv;
        if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EAM::GetUserRequest>(jv);
        if (request.userId.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request, "userId is required");
        }

        const auto user = Database::RepositoryFactory::instance().eamRepository()->findUserByUserId(request.userId);
        if (!user.has_value()) {
            return EamServer::ErrorResponse(req, status::not_found, "User not found, userId: " + request.userId);
        }

        log_debug << "Access GetUser, userId: " << request.userId;

        Dto::EAM::GetUserResponse response;
        response.user = Dto::EAM::EamMapper::toDto(*user);

        return EamServer::JsonResponse(req, status::ok, response.toJson());
    }

    // Reads one user group, by name or by ERN - the two things that name one. Groups are
    // installation-wide, so a name is unambiguous on its own.
    static response<string_body> handleGetUserGroup(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-user-group");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);
        if (!isAdmin(*auth.user)) {
            return EamServer::ErrorResponse(req, status::forbidden, "Administrator privileges required");
        }

        boost::json::value jv;
        if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EAM::GetUserGroupRequest>(jv);
        if (request.ern.empty() && request.name.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request, "Either ern or name is required");
        }

        const auto repo = Database::RepositoryFactory::instance().eamRepository();
        const auto group = request.ern.empty() ? repo->findUserGroupByName(request.name)
                                               : repo->findUserGroupByErn(request.ern);
        if (!group.has_value()) {
            return EamServer::ErrorResponse(req, status::not_found,
                                            request.ern.empty() ? "User group not found, name: " + request.name
                                                                : "User group not found, ern: " + request.ern);
        }

        log_debug << "Access GetUserGroup, name: " << group->name;

        Dto::EAM::GetUserGroupResponse response;
        response.userGroup = Dto::EAM::EamMapper::toDto(*group);

        return EamServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleListUsers(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-users");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) {
            return unauthorized(req, auth);
        }
        if (!isAdmin(*auth.user)) {
            return EamServer::ErrorResponse(req, status::forbidden, "Administrator privileges required");
        }

        const auto repo = Database::RepositoryFactory::instance().eamRepository();

        boost::json::value jv;
        if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EAM::ListUserRequest>(jv);

        const std::vector<Database::Entity::EAM::User> users = repo->listUsers(request.prefix, request.pageSize, request.pageIndex, request.sortColumn, request.sortDirection);
        log_info << "Access ListUsers" << (!request.prefix.empty() ? ", prefix: " + request.prefix : "");

        Dto::EAM::ListUserResponse response;
        response.users = Dto::EAM::EamMapper::toDto(users);
        response.total = repo->countUsers();
        return EamServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::value_from(response)));
    }

    // Deletes a user. Requires the caller to be an authenticated administrator - unlike
    // handleRegister() there's no bootstrap exception, since deleting requires an existing admin.
    static response<string_body> handleDeleteUser(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-user");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) {
            return unauthorized(req, auth);
        }
        if (!isAdmin(*auth.user)) {
            return EamServer::ErrorResponse(req, status::forbidden, "Administrator privileges required");
        }

        const auto repo = Database::RepositoryFactory::instance().eamRepository();

        boost::json::value jv;
        if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EAM::DeleteUserRequest>(jv);
        if (request.userId.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request, "userId is required");
        }

        if (!repo->userExists(request.userId)) {
            return EamServer::ErrorResponse(req, status::not_found, "User not found");
        }

        repo->deleteUser(request.userId);
        log_info << "User deleted, userId: " << request.userId;

        return EamServer::JsonResponse(req, status::ok);
    }

    /**
     * @brief Gives a user a different user ID.
     *
     * @par
     * Administrator-only, and about somebody rather than about yourself: unlike change-password,
     * which means "mine" when it names nobody, this one names both ids outright. A rename that
     * could mean the caller by saying nothing is one keystroke from renaming the wrong account.
     *
     * @par What moves with the name
     * The grants and the group memberships, in the repository - see IEamRepository::renameUser(),
     * which explains what does not move and why. What this handler adds is the refusals: a name
     * somebody else holds, and a name an application is running as.
     *
     * @par The application check
     * An application records the identity it runs as by userId, and the manager looks that identity
     * up on every reconcile to hand the process its credentials. Rename the user out from under it
     * and the lookup finds nothing: the application keeps running, gets no credentials, and is
     * refused by everything it calls - with one warning in the manager's log to say so. Refused
     * here instead, naming the application, because an operator who is told can point the
     * application somewhere else first and an application that is quietly broken cannot.
     *
     * @par What a rename does not do
     * It does not end a session. A bearer token is a JWT carrying the old id as its subject and is
     * verified against the signing secret rather than against anything stored, so a session already
     * open goes on working until it expires - and then cannot be refreshed, because the subject no
     * longer resolves. The same bargain change-password makes, for the same reason.
     */
    static response<string_body> handleChangeUserId(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "change-userid");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) {
            return unauthorized(req, auth);
        }
        if (!isAdmin(*auth.user)) {
            return EamServer::ErrorResponse(req, status::forbidden, "Administrator privileges required");
        }

        boost::json::value jv;
        if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EAM::ChangeUserIdRequest>(jv);
        if (request.userId.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request, "userId is required");
        }
        if (request.newUserId.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request, "newUserId is required");
        }
        if (request.userId == request.newUserId) {
            // Refused rather than answered as a success that changed nothing: the two ids being
            // the same is a caller who filled the same value in twice, not an idempotent retry -
            // a retry of a rename that went through names an id that no longer exists and is
            // answered 404 here, which is the truthful answer to it.
            return EamServer::ErrorResponse(req, status::bad_request, "newUserId is the id the user already has: " + request.userId);
        }

        const auto repo = Database::RepositoryFactory::instance().eamRepository();
        if (!repo->userExists(request.userId)) {
            return EamServer::ErrorResponse(req, status::not_found, "User not found, userId: " + request.userId);
        }
        if (repo->userExists(request.newUserId)) {
            return EamServer::ErrorResponse(req, status::conflict, "User already exists, userId: " + request.newUserId);
        }

        // Every application in the installation rather than the caller's namespace: an identity is
        // installation-wide - the unique index on userId says so - so an application in a namespace
        // this administrator never looks at is still an application this rename would break.
        for (const auto &application: Database::RepositoryFactory::instance().eapRepository()->listAllApplications("")) {
            if (application.userId != request.userId) continue;

            return EamServer::ErrorResponse(req, status::conflict,
                                            "User '" + request.userId + "' is the identity application '" + application.applicationId +
                                                    "' runs as. Point it at another user with 'eap update-application --user' first, "
                                                    "or the application would keep running and be refused everything it calls.");
        }

        const auto renamed = repo->renameUser(request.userId, request.newUserId);
        if (!renamed.has_value()) {
            // The user was there a moment ago - something else renamed or deleted them in between.
            return EamServer::ErrorResponse(req, status::conflict, "User could not be renamed, userId: " + request.userId);
        }

        log_info << "User renamed, userId: " << request.userId << " -> " << renamed->userId << ", by: " << auth.user->userId;

        Dto::EAM::GetUserResponse response;
        response.user = Dto::EAM::EamMapper::toDto(*renamed);

        return EamServer::JsonResponse(req, status::ok, response.toJson());
    }

    // Replaces a password. Two things wear this one name, and what separates them is what each
    // has to prove: a person changing their own proves it by knowing the old one, and an
    // administrator resetting somebody else's proves it by being an administrator.
    //
    // They are one action rather than two because they end in the same write, and two handlers
    // over one field is two chances for one of them to skip a check the other makes. Which of the
    // two a request is, is decided by the userId it names and not by what it sends - so a caller
    // cannot get the reset path by simply omitting the old password.
    //
    // Note what this does not do: the bearer token a session already holds is a JWT, verified
    // against the signing secret rather than against anything stored, so it stays valid for the
    // rest of its hour. Changing a password closes the door for the next login, not for a session
    // already through it.
    static response<string_body> handleChangePassword(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "change-password");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) {
            return unauthorized(req, auth);
        }

        boost::json::value jv;
        if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EAM::ChangePasswordRequest>(jv);
        if (request.newPassword.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request, "newPassword is required");
        }

        // Empty means "mine", so a client that knows only its own session does not have to name
        // itself - and naming yourself is the same request, not the administrator one.
        const auto targetUserId = request.userId.empty() ? auth.user->userId : request.userId;
        const bool ownPassword = targetUserId == auth.user->userId;

        if (!ownPassword && !isAdmin(*auth.user)) {
            return EamServer::ErrorResponse(req, status::forbidden, "Administrator privileges required to change another user's password");
        }

        const auto repo = Database::RepositoryFactory::instance().eamRepository();
        auto user = ownPassword ? auth.user : repo->findUserByUserId(targetUserId);
        if (!user.has_value()) {
            return EamServer::ErrorResponse(req, status::not_found, "User not found, userId: " + targetUserId);
        }

        // An account that does not log in with a password has no password to change, and giving
        // it one would be handing it a way in it was deliberately created without - the same flag
        // that keeps a federated user and an application's technical principal out of doLogin().
        if (!user->loginEnabled) {
            return EamServer::ErrorResponse(req, status::conflict, "User does not log in with a password, userId: " + targetUserId);
        }

        // Without this, a token left behind on an unattended terminal is enough to take an account
        // over rather than merely to use it until the hour is out.
        //
        // Refused as 403 rather than 401, though it is a credential that failed: 401 is what every
        // euclid client is told means "your session is gone", and answers it by throwing the stored
        // one away and asking for a login. A mistyped old password would then log the caller out,
        // which is a poor answer to a typo. The session is fine; this one request is not.
        if (ownPassword && !Core::PasswordUtils::Verify(request.oldPassword, user->password)) {
            log_warning << "Password change refused, the old password does not match, userId: " << user->userId;
            return EamServer::ErrorResponse(req, status::forbidden, "The old password is not correct");
        }

        user->password = Core::PasswordUtils::Hash(request.newPassword);
        user->modified = std::chrono::system_clock::now();
        repo->upsertUser(*user);

        log_info << "Password changed, userId: " << user->userId
                 << (ownPassword ? "" : ", reset by: " + auth.user->userId);

        return EamServer::JsonResponse(req, status::ok);
    }

    // Creates a new access key for the authenticated caller. Self-service - unlike
    // handleListUsers/handleDeleteUser there's no admin requirement, since a user managing
    // their own signing credentials doesn't need elevated privileges.
    static response<string_body> handleCreateAccessKey(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "create-access-key");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) {
            return unauthorized(req, auth);
        }

        Database::Entity::EAM::AccessKey key;
        key.accessKeyId = Core::CryptoUtils::GenerateAccessKeyId();
        key.secretAccessKey = Core::CryptoUtils::GenerateSecretAccessKey();
        key.active = true;
        key.created = Core::DateTimeUtils::ToISO8601(Core::DateTimeUtils::UtcDateTimeNow());

        auto user = *auth.user;
        user.accessKeys.push_back(key);
        Database::RepositoryFactory::instance().eamRepository()->upsertUser(user);
        log_info << "Access key created, userId: " << user.userId << ", accessKeyId: " << key.accessKeyId;

        Dto::EAM::CreateAccessKeyResponse response;
        response.accessKeyId = key.accessKeyId;
        response.secretAccessKey = key.secretAccessKey;
        response.createdAt = key.created;
        return EamServer::JsonResponse(req, status::created, response.toJson());
    }

    // Lists the authenticated caller's own access keys (never including the secret).
    static response<string_body> handleListAccessKeys(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-access-keys");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) {
            return unauthorized(req, auth);
        }

        Dto::EAM::ListAccessKeysResponse response;
        for (const auto &key: auth.user->accessKeys) {
            response.accessKeys.push_back({.accessKeyId = key.accessKeyId, .active = key.active, .createdAt = key.created});
        }
        return EamServer::JsonResponse(req, status::ok, response.toJson());
    }

    // Deletes one of the authenticated caller's own access keys.
    static response<string_body> handleDeleteAccessKey(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-access-key");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) {
            return unauthorized(req, auth);
        }

        boost::json::value jv;
        if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EAM::DeleteAccessKeyRequest>(jv);
        if (request.accessKeyId.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request, "accessKeyId is required");
        }

        auto user = *auth.user;
        const auto before = user.accessKeys.size();
        std::erase_if(user.accessKeys, [&](const auto &key) { return key.accessKeyId == request.accessKeyId; });
        if (user.accessKeys.size() == before) {
            return EamServer::ErrorResponse(req, status::not_found, "Access key not found");
        }

        Database::RepositoryFactory::instance().eamRepository()->upsertUser(user);
        log_info << "Access key deleted, userId: " << user.userId << ", accessKeyId: " << request.accessKeyId;

        return EamServer::JsonResponse(req, status::ok);
    }

    static response<string_body> handleCreateUserGroup(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "create-user-group");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) {
            return unauthorized(req, auth);
        }
        if (!isAdmin(*auth.user)) {
            return EamServer::ErrorResponse(req, status::forbidden, "Administrator privileges required");
        }

        boost::json::value jv;
        if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EAM::CreateUserGroupRequest>(jv);
        if (request.name.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request, "name is required");
        }

        const auto repo = Database::RepositoryFactory::instance().eamRepository();
        if (repo->userGroupExists(request.name)) {
            return EamServer::ErrorResponse(req, status::conflict, "User group already exists");
        }

        Database::Entity::EAM::UserGroup group;
        group.name = request.name;
        group.description = request.description;
        group.accountId = auth.user->accountId;
        group.region = auth.user->region;
        group.ern = Core::createEamUserGroupErn(auth.user->accountId, request.name);
        group.created = std::chrono::system_clock::now();
        group.modified = std::chrono::system_clock::now();

        const auto saved = repo->upsertUserGroup(group);
        log_info << "User group created, name: " << saved.name << ", accountId: " << saved.accountId;

        Dto::EAM::CreateUserGroupResponse response;
        response.userGroup = Dto::EAM::EamMapper::toDto(saved);
        return EamServer::JsonResponse(req, status::created, response.toJson());
    }

    static response<string_body> handleUserGroupAddUser(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "user-group-add-user");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) {
            return unauthorized(req, auth);
        }
        if (!isAdmin(*auth.user)) {
            return EamServer::ErrorResponse(req, status::forbidden, "Administrator privileges required");
        }

        boost::json::value jv;
        if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EAM::UserGroupAddUserRequest>(jv);
        if (request.userGroup.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request, "user group ERN is required");
        }
        if (request.user.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request, "user ERN is required");
        }

        const auto repo = Database::RepositoryFactory::instance().eamRepository();
        if (!repo->userGroupErnExists(request.userGroup)) {
            return EamServer::ErrorResponse(req, status::conflict, "User group does not exist");
        }
        if (!repo->userErnExists(request.user)) {
            return EamServer::ErrorResponse(req, status::conflict, "User does not exist");
        }

        std::optional<Database::Entity::EAM::UserGroup> group = repo->findUserGroupByErn(request.userGroup);
        std::optional<Database::Entity::EAM::User> user = repo->findUserByErn(request.user);

        if (std::ranges::contains(group->userIds, user->userId)) {
            return EamServer::ErrorResponse(req, status::conflict, "User already member of user group");
        }
        group->userIds.push_back(user->userId);

        const auto saved = repo->upsertUserGroup(group.value());
        log_info << "User added to user group, userGroup: " << saved.name << ", user: " << user->userId;

        return EamServer::JsonResponse(req, status::ok);
    }

    static response<string_body> handleUserGroupRemoveUser(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "user-group-remove-user");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) {
            return unauthorized(req, auth);
        }
        if (!isAdmin(*auth.user)) {
            return EamServer::ErrorResponse(req, status::forbidden, "Administrator privileges required");
        }

        boost::json::value jv;
        if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EAM::UserGroupAddUserRequest>(jv);
        if (request.userGroup.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request, "user group ERN is required");
        }
        if (request.user.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request, "user ERN is required");
        }

        const auto repo = Database::RepositoryFactory::instance().eamRepository();
        if (!repo->userGroupErnExists(request.userGroup)) {
            return EamServer::ErrorResponse(req, status::conflict, "User group does not exist");
        }
        if (!repo->userErnExists(request.user)) {
            return EamServer::ErrorResponse(req, status::conflict, "User does not exist");
        }

        std::optional<Database::Entity::EAM::UserGroup> group = repo->findUserGroupByErn(request.userGroup);
        std::optional<Database::Entity::EAM::User> user = repo->findUserByErn(request.user);

        std::erase(group->userIds, user->userId);

        const auto saved = repo->upsertUserGroup(group.value());
        log_info << "User removed from user group, userGroup: " << saved.name << ", user: " << user->userId;

        return EamServer::JsonResponse(req, status::ok);
    }

    static response<string_body> handleListUserGroups(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-user-groups");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) {
            return unauthorized(req, auth);
        }
        if (!isAdmin(*auth.user)) {
            return EamServer::ErrorResponse(req, status::forbidden, "Administrator privileges required");
        }

        const auto repo = Database::RepositoryFactory::instance().eamRepository();

        boost::json::value jv;
        if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EAM::ListUserGroupsRequest>(jv);

        const std::vector<Database::Entity::EAM::UserGroup> userGroups = repo->listUserGroups(request.prefix, request.pageSize, request.pageIndex, request.sortColumn, request.sortDirection);
        log_info << "EAM ListUserGroups" << (!request.prefix.empty() ? ", prefix: " + request.prefix : "");

        Dto::EAM::ListUserGroupsResponse response;
        response.userGroups = Dto::EAM::EamMapper::toDto(userGroups);
        response.total = repo->countUserGroups();
        return EamServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::value_from(response)));
    }

    static response<string_body> handleDeleteUserGroup(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-user-group");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) {
            return unauthorized(req, auth);
        }
        if (!isAdmin(*auth.user)) {
            return EamServer::ErrorResponse(req, status::forbidden, "Administrator privileges required");
        }

        boost::json::value jv;
        if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EAM::DeleteUserGroupRequest>(jv);
        if (request.name.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request, "name is required");
        }

        const auto repo = Database::RepositoryFactory::instance().eamRepository();
        if (!repo->userGroupExists(request.name)) {
            return EamServer::ErrorResponse(req, status::conflict, "User group does not exist");
        }

        repo->deleteUserGroup(request.name);
        log_info << "User group deleted, name: " << request.name;

        return EamServer::JsonResponse(req, status::ok);
    }

    static response<string_body> handleCreateAccount(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "create-account");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) {
            return unauthorized(req, auth);
        }
        if (!isAdmin(*auth.user)) {
            return EamServer::ErrorResponse(req, status::forbidden, "Administrator privileges required");
        }

        boost::json::value jv;
        if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EAM::CreateAccountRequest>(jv);
        if (request.accountId.empty() || request.name.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request, "accountId and name are required");
        }

        const auto repo = Database::RepositoryFactory::instance().eamRepository();
        if (repo->accountExists(request.accountId)) {
            return EamServer::ErrorResponse(req, status::conflict, "Account already exists");
        }

        Database::Entity::EAM::Account account;
        account.accountId = request.accountId;
        account.name = request.name;
        account.description = request.description;
        account.ern = Core::createEamAccountErn(request.accountId);
        account.created = std::chrono::system_clock::now();
        account.modified = std::chrono::system_clock::now();

        const auto saved = repo->upsertAccount(account);
        log_info << "Account created, accountId: " << saved.accountId;

        Dto::EAM::CreateAccountResponse response;
        response.account = Dto::EAM::EamMapper::toDto(saved);
        return EamServer::JsonResponse(req, status::created, response.toJson());
    }

    static response<string_body> handleListAccounts(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-accounts");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) {
            return unauthorized(req, auth);
        }
        if (!isAdmin(*auth.user)) {
            return EamServer::ErrorResponse(req, status::forbidden, "Administrator privileges required");
        }

        const auto repo = Database::RepositoryFactory::instance().eamRepository();

        boost::json::value jv;
        if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EAM::ListAccountsRequest>(jv);

        const auto accounts = repo->listAccounts(request.prefix, request.pageSize, request.pageIndex, request.sortColumn, request.sortDirection);
        log_info << "EAM ListAccounts" << (!request.prefix.empty() ? ", prefix: " + request.prefix : "");

        Dto::EAM::ListAccountsResponse response;
        response.accounts = Dto::EAM::EamMapper::toDto(accounts);
        response.total = repo->countAccounts();
        return EamServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::value_from(response)));
    }

    // Reads one account, by account ID or by ERN - the two things that name one. Administrator-only,
    // like the listing it answers a single row of.
    static response<string_body> handleGetAccount(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-account");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);
        if (!isAdmin(*auth.user)) {
            return EamServer::ErrorResponse(req, status::forbidden, "Administrator privileges required");
        }

        boost::json::value jv;
        if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EAM::GetAccountRequest>(jv);
        if (request.accountId.empty() && request.ern.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request, "Either accountId or ern is required");
        }

        const auto repo = Database::RepositoryFactory::instance().eamRepository();
        const auto account = request.accountId.empty() ? repo->findAccountByErn(request.ern)
                                                       : repo->findAccountByAccountId(request.accountId);
        if (!account.has_value()) {
            return EamServer::ErrorResponse(req, status::not_found,
                                            request.accountId.empty() ? "Account not found, ern: " + request.ern
                                                                      : "Account not found, accountId: " + request.accountId);
        }

        log_debug << "Access GetAccount, accountId: " << account->accountId;

        Dto::EAM::GetAccountResponse response;
        response.account = Dto::EAM::EamMapper::toDto(*account);

        return EamServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleDeleteAccount(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-account");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) {
            return unauthorized(req, auth);
        }
        if (!isAdmin(*auth.user)) {
            return EamServer::ErrorResponse(req, status::forbidden, "Administrator privileges required");
        }

        boost::json::value jv;
        if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EAM::DeleteAccountRequest>(jv);
        if (request.accountId.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request, "accountId is required");
        }

        const auto repo = Database::RepositoryFactory::instance().eamRepository();
        if (!repo->accountExists(request.accountId)) {
            return EamServer::ErrorResponse(req, status::not_found, "Account not found");
        }
        if (repo->countNamespaces(request.accountId) > 0) {
            return EamServer::ErrorResponse(req, status::conflict, "Account still has namespaces, delete those first");
        }

        // Every grant in the account, whoever holds it. Asked per role rather than per user
        // because that is the index the grant store has - and an account with no roles left has
        // nothing granted in it by definition.
        const bool stillGranted = std::ranges::any_of(Core::BuiltinRoles::Names(), [&](const auto &role) {
                                      return !repo->findGrantsByRole(request.accountId, role).empty();
                                  }) ||
                                  std::ranges::any_of(repo->listRoles(request.accountId, "", 0, 0, "name"), [&](const auto &role) {
                                      return !repo->findGrantsByRole(request.accountId, role.name).empty();
                                  });
        if (stillGranted) {
            return EamServer::ErrorResponse(req, status::conflict, "Account still has role grants, revoke those first");
        }

        repo->deleteAccount(request.accountId);
        log_info << "Account deleted, accountId: " << request.accountId;

        return EamServer::JsonResponse(req, status::ok);
    }

    static response<string_body> handleCreateNamespace(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "create-namespace");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) {
            return unauthorized(req, auth);
        }

        boost::json::value jv;
        if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EAM::CreateNamespaceRequest>(jv);
        if (request.accountId.empty() || request.name.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request, "accountId and name are required");
        }
        if (!isAccountAdmin(*auth.user, request.accountId)) {
            return EamServer::ErrorResponse(req, status::forbidden, "Administrator privileges required for this account");
        }

        const auto repo = Database::RepositoryFactory::instance().eamRepository();
        if (!repo->accountExists(request.accountId)) {
            return EamServer::ErrorResponse(req, status::conflict, "Account does not exist");
        }
        if (repo->namespaceExists(request.accountId, request.name)) {
            return EamServer::ErrorResponse(req, status::conflict, "Namespace already exists");
        }

        Database::Entity::EAM::Namespace ns;
        ns.accountId = request.accountId;
        ns.name = request.name;
        ns.description = request.description;
        ns.ern = Core::createEamNamespaceErn(request.accountId, request.name);
        ns.created = std::chrono::system_clock::now();
        ns.modified = std::chrono::system_clock::now();

        const auto saved = repo->upsertNamespace(ns);
        log_info << "Namespace created, accountId: " << saved.accountId << ", name: " << saved.name;

        Dto::EAM::CreateNamespaceResponse response;
        response.ns = Dto::EAM::EamMapper::toDto(saved);
        return EamServer::JsonResponse(req, status::created, response.toJson());
    }

    static response<string_body> handleListNamespaces(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-namespaces");

        if (const auto auth = authenticate(req); !auth.user.has_value()) {
            return unauthorized(req, auth);
        }

        boost::json::value jv;
        if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EAM::ListNamespacesRequest>(jv);
        if (request.accountId.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request, "accountId is required");
        }
        // if (!isAccountAdmin(*auth.user, request.accountId)) {
        //     return EamServer::ErrorResponse(req, status::forbidden, "Administrator privileges required for this account");
        // }

        const auto repo = Database::RepositoryFactory::instance().eamRepository();
        const auto namespaces = repo->listNamespaces(request.accountId, request.prefix, request.pageSize, request.pageIndex, request.sortColumn, request.sortDirection);
        log_info << "EAM ListNamespaces, accountId: " << request.accountId << (!request.prefix.empty() ? ", prefix: " + request.prefix : "");

        Dto::EAM::ListNamespacesResponse response;
        response.namespaces = Dto::EAM::EamMapper::toDto(namespaces);
        response.total = repo->countNamespaces(request.accountId);
        return EamServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::value_from(response)));
    }

    static response<string_body> handleDeleteNamespace(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-namespace");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) {
            return unauthorized(req, auth);
        }

        boost::json::value jv;
        if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EAM::DeleteNamespaceRequest>(jv);
        if (request.accountId.empty() || request.name.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request, "accountId and name are required");
        }
        if (!isAccountAdmin(*auth.user, request.accountId)) {
            return EamServer::ErrorResponse(req, status::forbidden, "Administrator privileges required for this account");
        }

        const auto repo = Database::RepositoryFactory::instance().eamRepository();
        if (!repo->namespaceExists(request.accountId, request.name)) {
            return EamServer::ErrorResponse(req, status::not_found, "Namespace not found");
        }

        // A grant scoped to this namespace by name. One scoped to "*" is not counted: it covers
        // whatever namespaces the account has, and would make every namespace undeletable.
        const auto grantsNaming = [&](const std::string &role) {
            return std::ranges::any_of(repo->findGrantsByRole(request.accountId, role), [&](const auto &grant) {
                return std::ranges::contains(grant.namespaces, request.name);
            });
        };
        const bool stillGranted = std::ranges::any_of(Core::BuiltinRoles::Names(), grantsNaming) ||
                                  std::ranges::any_of(repo->listRoles(request.accountId, "", 0, 0, "name"),
                                                      [&](const auto &role) { return grantsNaming(role.name); });
        if (stillGranted) {
            return EamServer::ErrorResponse(req, status::conflict, "Namespace still has role grants, revoke those first");
        }

        repo->deleteNamespace(request.accountId, request.name);
        log_info << "Namespace deleted, accountId: " << request.accountId << ", name: " << request.name;

        return EamServer::JsonResponse(req, status::ok);
    }

    // Switches the caller's active namespace (persisted client-side in ~/.euclid/credentials,
    // sent back on every subsequent request as x-euclid-namespace). An empty namespace clears it
    // back to unscoped and always succeeds; a non-empty one must exist and the caller must either
    // be an account admin or hold an explicit grant for it (see handleGrantNamespaceAccess) -
    // otherwise a user could silently point every future command at a namespace they have no
    // business touching.
    static response<string_body> handleChangeNamespace(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "change-namespace");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EAM::ChangeNamespaceRequest>(jv);

        if (request.ns.empty()) {
            log_info << "EAM ChangeNamespace, userId: " << auth.user->userId << ", namespace: (cleared)";
            return EamServer::JsonResponse(req, status::ok);
        }

        const auto repo = Database::RepositoryFactory::instance().eamRepository();
        if (!repo->namespaceExists(auth.user->accountId, request.ns)) {
            return EamServer::ErrorResponse(req, status::not_found, "Namespace does not exist");
        }

        const bool granted = isAccountAdmin(*auth.user, auth.user->accountId) ||
                             std::ranges::any_of(repo->findGrantsByPrincipals(Database::PrincipalsOf(*auth.user)),
                                                 [&](const auto &grant) {
                                                     return grant.accountId == auth.user->accountId &&
                                                            (std::ranges::contains(grant.namespaces, request.ns) ||
                                                             std::ranges::contains(grant.namespaces, std::string("*")));
                                                 });
        if (!granted) {
            return EamServer::ErrorResponse(req, status::forbidden, "Namespace access not granted");
        }

        log_info << "EAM ChangeNamespace, userId: " << auth.user->userId << ", namespace: " << request.ns;
        return EamServer::JsonResponse(req, status::ok);
    }

    // ── Request dispatcher ───────────────────────────────────────────────────

    namespace {
        // Actions the access service accepts via the "x-euclid-action" header.

    // ── Roles and grants ─────────────────────────────────────────────────────
    //
    // Administrator-only for now, the way every other access-management action here is: these
    // decide what everybody else may do, and until the gate exists (docs/role-concept.md §9 step 3)
    // there is no role that could be granted to delegate them safely.

    // A role as the API reports it. Stored roles and built-in ones answer the same shape, with
    // "builtin" as the only way to tell - so a caller knows which it may change without having to
    // know the six names.
    static Dto::EAM::Role toRoleDto(const Database::Entity::EAM::Role &role) {
        Dto::EAM::Role dto;
        dto.name = role.name;
        dto.ern = role.ern;
        dto.accountId = role.accountId;
        dto.region = role.region;
        dto.description = role.description;
        dto.permissions = role.permissions;
        dto.builtin = false;
        dto.created = Core::DateTimeUtils::ToISO8601(role.created);
        dto.modified = Core::DateTimeUtils::ToISO8601(role.modified);
        return dto;
    }

    static Dto::EAM::Role toBuiltinRoleDto(const std::string &name, const std::string &accountId, const std::string &region) {
        Dto::EAM::Role dto;
        dto.name = name;
        dto.ern = Core::createEamRoleErn(accountId, name);
        dto.accountId = accountId;
        dto.region = region;
        dto.description = Core::BuiltinRoles::DescriptionOf(name);
        dto.permissions = Core::BuiltinRoles::PermissionsOf(name);
        dto.builtin = true;
        return dto;
    }

    static Dto::EAM::Grant toGrantDto(const Database::Entity::EAM::Grant &grant) {
        Dto::EAM::Grant dto;
        dto.grantId = grant.oid;
        dto.role = grant.role;
        dto.principal = grant.principal;
        dto.accountId = grant.accountId;
        dto.namespaces = grant.namespaces;
        dto.resources = grant.resources;
        dto.granted = Core::DateTimeUtils::ToISO8601(grant.granted);
        dto.grantedBy = grant.grantedBy;
        return dto;
    }

    // Every permission a role names has to exist. A role holding "ens:publish-mesage" would grant
    // nothing and say so to nobody, which is worse than being refused: the administrator believes
    // they granted something and the user believes they were not.
    static std::optional<std::string> invalidPermission(const std::vector<std::string> &permissions) {
        for (const auto &permission: permissions) {
            if (permission == Core::Permissions::Everything) continue;
            if (permission.ends_with(":*")) {
                if (const auto module = permission.substr(0, permission.size() - 2); Core::Permissions::IsBindable(module)) continue;
                return permission;
            }
            if (!Core::Permissions::Exists(permission)) return permission;
        }
        return std::nullopt;
    }

    // Every ERN a caller's grants can hang off: their own, and each group they belong to. Their
    // rights are the union of all of them, which is why this is one list and one query rather than
    // a loop of lookups.
    static std::vector<std::string> principalsOf(const Database::Entity::EAM::User &user) {

        std::vector<std::string> principals{user.ern};

        // Groups are installation-wide, so this asks for all of them. The set is small - a handful
        // per installation - and this runs on check-permission, not on every request.
        for (const auto repo = Database::RepositoryFactory::instance().eamRepository();
             const auto &group: repo->listUserGroups("", 0, 0, "name")) {
            if (std::ranges::contains(group.userIds, user.userId)) principals.push_back(group.ern);
        }
        return principals;
    }

    static response<string_body> handleCreateRole(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "create-role");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);
        if (!isAdmin(*auth.user)) {
            return EamServer::ErrorResponse(req, status::forbidden, "Administrator privileges required");
        }

        boost::json::value jv;
        if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EAM::CreateRoleRequest>(jv);
        if (request.name.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request, "name is required");
        }
        if (request.permissions.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request, "a role with no permissions grants nothing; give it at least one");
        }
        // A stored role may not shadow a built-in one: a grant resolves the account's own roles
        // first, so it would silently replace the built-in for that account alone.
        if (Core::BuiltinRoles::Exists(request.name)) {
            return EamServer::ErrorResponse(req, status::conflict, "'" + request.name + "' is a built-in role and cannot be redefined");
        }
        if (const auto bad = invalidPermission(request.permissions)) {
            return EamServer::ErrorResponse(req, status::bad_request,
                                            "'" + *bad + "' is not a permission any module answers - see list-permissions");
        }

        const auto repo = Database::RepositoryFactory::instance().eamRepository();
        if (repo->findRoleByName(auth.user->accountId, request.name).has_value()) {
            return EamServer::ErrorResponse(req, status::conflict, "Role already exists");
        }

        Database::Entity::EAM::Role role;
        role.name = request.name;
        role.description = request.description;
        role.permissions = request.permissions;
        role.accountId = auth.user->accountId;
        role.region = auth.user->region;
        role.ern = Core::createEamRoleErn(auth.user->accountId, request.name);
        role.created = std::chrono::system_clock::now();
        role.modified = role.created;

        const auto saved = repo->upsertRole(role);
        log_info << "Role created, name: " << saved.name << ", accountId: " << saved.accountId << ", permissions: " << saved.permissions.size();

        Dto::EAM::RoleResponse response;
        response.role = toRoleDto(saved);
        return EamServer::JsonResponse(req, status::created, response.toJson());
    }

    static response<string_body> handleUpdateRole(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "update-role");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);
        if (!isAdmin(*auth.user)) {
            return EamServer::ErrorResponse(req, status::forbidden, "Administrator privileges required");
        }

        boost::json::value jv;
        if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EAM::UpdateRoleRequest>(jv);
        if (request.name.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request, "name is required");
        }
        if (Core::BuiltinRoles::Exists(request.name)) {
            return EamServer::ErrorResponse(req, status::forbidden, "'" + request.name + "' is a built-in role and cannot be changed");
        }
        if (request.permissions.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request, "a role with no permissions grants nothing; give it at least one");
        }
        if (const auto bad = invalidPermission(request.permissions)) {
            return EamServer::ErrorResponse(req, status::bad_request,
                                            "'" + *bad + "' is not a permission any module answers - see list-permissions");
        }

        const auto repo = Database::RepositoryFactory::instance().eamRepository();
        auto existing = repo->findRoleByName(auth.user->accountId, request.name);
        if (!existing.has_value()) {
            return EamServer::ErrorResponse(req, status::not_found, "Role not found, name: " + request.name);
        }

        // Replaces rather than merges - a permission left out is taken away, which is the only way
        // to narrow a role at all.
        existing->description = request.description;
        existing->permissions = request.permissions;
        existing->modified = std::chrono::system_clock::now();

        const auto saved = repo->upsertRole(*existing);
        log_info << "Role updated, name: " << saved.name << ", accountId: " << saved.accountId << ", permissions: " << saved.permissions.size();

        Dto::EAM::RoleResponse response;
        response.role = toRoleDto(saved);
        return EamServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleGetRole(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-role");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);
        if (!isAdmin(*auth.user)) {
            return EamServer::ErrorResponse(req, status::forbidden, "Administrator privileges required");
        }

        boost::json::value jv;
        if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EAM::GetRoleRequest>(jv);
        if (request.name.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request, "name is required");
        }

        Dto::EAM::RoleResponse response;

        // The account's own first, then the built-ins - the same order a grant resolves in, so what
        // this shows is what a grant of that name would actually use.
        if (const auto stored = Database::RepositoryFactory::instance().eamRepository()->findRoleByName(auth.user->accountId, request.name)) {
            response.role = toRoleDto(*stored);
        } else if (Core::BuiltinRoles::Exists(request.name)) {
            response.role = toBuiltinRoleDto(request.name, auth.user->accountId, auth.user->region);
        } else {
            return EamServer::ErrorResponse(req, status::not_found, "Role not found, name: " + request.name);
        }

        return EamServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleListRoles(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-roles");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);
        if (!isAdmin(*auth.user)) {
            return EamServer::ErrorResponse(req, status::forbidden, "Administrator privileges required");
        }

        boost::json::value jv;
        if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EAM::ListRolesRequest>(jv);
        const auto repo = Database::RepositoryFactory::instance().eamRepository();

        Dto::EAM::ListRolesResponse response;

        // Built-in roles first, and outside the paging: they are not stored, so there is no page to
        // put them on, and a caller listing roles almost always wants to see what it can bind.
        if (request.includeBuiltin) {
            for (const auto &name: Core::BuiltinRoles::Names()) {
                if (!request.prefix.empty() && !name.starts_with(request.prefix)) continue;
                response.roles.push_back(toBuiltinRoleDto(name, auth.user->accountId, auth.user->region));
            }
        }

        for (const auto &role: repo->listRoles(auth.user->accountId, request.prefix, request.pageSize, request.pageIndex,
                                               request.sortColumn, request.sortDirection)) {
            response.roles.push_back(toRoleDto(role));
        }
        response.total = repo->countRoles(auth.user->accountId);

        return EamServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleDeleteRole(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-role");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);
        if (!isAdmin(*auth.user)) {
            return EamServer::ErrorResponse(req, status::forbidden, "Administrator privileges required");
        }

        boost::json::value jv;
        if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EAM::DeleteRoleRequest>(jv);
        if (request.name.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request, "name is required");
        }
        if (Core::BuiltinRoles::Exists(request.name)) {
            return EamServer::ErrorResponse(req, status::forbidden, "'" + request.name + "' is a built-in role and cannot be deleted");
        }

        const auto repo = Database::RepositoryFactory::instance().eamRepository();
        if (!repo->findRoleByName(auth.user->accountId, request.name).has_value()) {
            return EamServer::ErrorResponse(req, status::not_found, "Role not found, name: " + request.name);
        }

        // Refused rather than cascading. Deleting a role out from under its grants leaves grants
        // that quietly do nothing, and revoking somebody's access is a decision to take on purpose.
        if (const auto grants = repo->findGrantsByRole(auth.user->accountId, request.name); !grants.empty()) {
            return EamServer::ErrorResponse(req, status::conflict,
                                            "Role is still granted to " + std::to_string(grants.size()) +
                                                    " principal(s); revoke those grants first - see list-grants --role");
        }

        repo->deleteRole(auth.user->accountId, request.name);
        log_info << "Role deleted, name: " << request.name << ", accountId: " << auth.user->accountId;

        return EamServer::JsonResponse(req, status::ok);
    }

    static response<string_body> handleGrantRole(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "grant-role");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);
        if (!isAdmin(*auth.user)) {
            return EamServer::ErrorResponse(req, status::forbidden, "Administrator privileges required");
        }

        boost::json::value jv;
        if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EAM::GrantRoleRequest>(jv);
        if (request.role.empty() || request.principal.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request, "role and principal are required");
        }
        // A grant that applies in no namespace grants nothing, which is a mistake rather than a
        // configuration - said now rather than stored as a silent no-op.
        if (request.namespaces.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request,
                                            R"(namespaces is required; use ["*"] for every namespace of the account)");
        }

        // The caller's own account unless they named one, and naming another is something only an
        // administrator of it may do - otherwise granting in an account would be a way into it.
        const auto accountId = request.accountId.empty() ? auth.user->accountId : request.accountId;
        if (accountId != auth.user->accountId && !isAccountAdmin(*auth.user, accountId)) {
            return EamServer::ErrorResponse(req, status::forbidden, "Not an administrator of account " + accountId);
        }

        const auto repo = Database::RepositoryFactory::instance().eamRepository();

        // The role has to exist, here, now - a grant naming a role that does not exist is inert,
        // and nobody would find out until somebody was refused something they were told they had.
        if (!repo->findRoleByName(accountId, request.role).has_value() && !Core::BuiltinRoles::Exists(request.role)) {
            return EamServer::ErrorResponse(req, status::not_found, "Role not found, name: " + request.role);
        }

        // And so does the principal. One field for users and groups, so which of the two is
        // decided by which lookup answers.
        const bool isUser = repo->findUserByErn(request.principal).has_value();
        if (!isUser && !repo->findUserGroupByErn(request.principal).has_value()) {
            return EamServer::ErrorResponse(req, status::not_found, "No user or user group with ERN " + request.principal);
        }

        Database::Entity::EAM::Grant grant;
        grant.role = request.role;
        grant.principal = request.principal;
        grant.accountId = accountId;
        grant.namespaces = request.namespaces;
        grant.resources = request.resources;
        grant.granted = std::chrono::system_clock::now();
        grant.grantedBy = auth.user->userId;

        const auto saved = repo->addGrant(grant);
        log_info << "Role granted, role: " << saved.role << ", principal: " << saved.principal
                 << ", accountId: " << saved.accountId << ", by: " << saved.grantedBy;

        Dto::EAM::GrantRoleResponse response;
        response.grant = toGrantDto(saved);
        return EamServer::JsonResponse(req, status::created, response.toJson());
    }

    static response<string_body> handleRevokeRole(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "revoke-role");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);
        if (!isAdmin(*auth.user)) {
            return EamServer::ErrorResponse(req, status::forbidden, "Administrator privileges required");
        }

        boost::json::value jv;
        if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EAM::RevokeRoleRequest>(jv);
        if (request.grantId.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request, "grantId is required - see list-grants");
        }

        Database::RepositoryFactory::instance().eamRepository()->deleteGrant(request.grantId);
        log_info << "Role revoked, grantId: " << request.grantId << ", by: " << auth.user->userId;

        return EamServer::JsonResponse(req, status::ok);
    }

    static response<string_body> handleListGrants(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-grants");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);
        if (!isAdmin(*auth.user)) {
            return EamServer::ErrorResponse(req, status::forbidden, "Administrator privileges required");
        }

        boost::json::value jv;
        if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EAM::ListGrantsRequest>(jv);
        if (!request.principal.empty() && !request.role.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request,
                                            "give principal or role, not both - 'what may they do' and 'who can do this' "
                                            "are different questions");
        }

        const auto accountId = request.accountId.empty() ? auth.user->accountId : request.accountId;
        if (accountId != auth.user->accountId && !isAccountAdmin(*auth.user, accountId)) {
            return EamServer::ErrorResponse(req, status::forbidden, "Not an administrator of account " + accountId);
        }

        const auto repo = Database::RepositoryFactory::instance().eamRepository();

        Dto::EAM::ListGrantsResponse response;
        // Neither is a third question - "what is granted here at all" - and the one an
        // administration view asks: a list of users and what each may do is otherwise one request
        // per user, which is what the per-user grant lists used to give away for free. That is
        // also the case worth paging: one row per principal per role adds up in a large account.
        const auto grants = repo->listGrants(request.principal, request.role, accountId,
                                             request.pageSize, request.pageIndex,
                                             request.sortColumn, request.sortDirection);

        for (const auto &grant: grants) response.grants.push_back(toGrantDto(grant));

        // The whole set under the same filter, not the page: a total that counted the page would
        // say nothing a caller cannot already see, and would say "10" forever while paging.
        response.total = repo->countGrants(request.principal, request.role, accountId);

        return EamServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleListPermissions(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-permissions");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        // Readable by anybody logged in, unlike the rest of these: it is the vocabulary, not
        // anybody's access, and a user asking what could be granted is asking nothing private.
        Dto::EAM::ListPermissionsResponse response;
        response.permissions = Core::Permissions::All();
        response.modules = Core::Permissions::Modules();
        response.unbindableModules = Core::Permissions::UnbindableModules();

        return EamServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleCheckPermission(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "check-permission");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);
        if (!isAdmin(*auth.user)) {
            return EamServer::ErrorResponse(req, status::forbidden, "Administrator privileges required");
        }

        boost::json::value jv;
        if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EAM::CheckPermissionRequest>(jv);
        if (request.userId.empty() || request.target.empty() || request.action.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request, "userId, target and action are required");
        }

        const auto repo = Database::RepositoryFactory::instance().eamRepository();
        const auto subject = repo->findUserByUserId(request.userId);
        if (!subject.has_value()) {
            return EamServer::ErrorResponse(req, status::not_found, "User not found, userId: " + request.userId);
        }

        Dto::EAM::CheckPermissionResponse response;

        // The two short-circuits, answered as such rather than silently: an installation
        // administrator is allowed everything without holding a single grant, and saying so is the
        // whole point of this action.
        if (Database::IsCachedEamAdmin(subject->userId)) {
            response.allowed = true;
            response.reason = "member of the administrator user group, which is allowed everything and holds no grants";
            return EamServer::JsonResponse(req, status::ok, response.toJson());
        }

        const auto result = Database::Authorization::Allows(
                {.target = request.target, .action = request.action, .accountId = subject->accountId,
                 .nameSpace = request.nameSpace, .resourceErn = request.resourceErn},
                repo->findGrantsByPrincipals(principalsOf(*subject)),
                [&repo](const std::string &accountId, const std::string &role) -> std::optional<std::vector<std::string>> {
                    if (const auto stored = repo->findRoleByName(accountId, role)) return stored->permissions;
                    if (Core::BuiltinRoles::Exists(role)) return Core::BuiltinRoles::PermissionsOf(role);
                    return std::nullopt;
                });

        response.allowed = result.allowed;
        response.reason = result.reason;
        response.role = result.role;

        return EamServer::JsonResponse(req, status::ok, response.toJson());
    }


        enum class Action {
            Unknown,
            Login,
            RefreshSession,
            OidcAuthorize,
            OidcLogin,
            SamlAuthorize,
            SamlAcs,
            SamlMetadata,
            Register,
            GetUser,
            GetUserGroup,
            ListUsers,
            DeleteUser,
            ChangePassword,
            CreateAccessKey,
            ListAccessKeys,
            DeleteAccessKey,
            CreateUserGroup,
            ListUserGroups,
            UserGroupAddUser,
            UserGroupRemoveUser,
            DeleteUserGroup,
            CreateAccount,
            GetAccount,
            ListAccounts,
            DeleteAccount,
            CreateNamespace,
            ListNamespaces,
            DeleteNamespace,
            ChangeNamespace,
            ChangeUserId,
            CreateRole,
            UpdateRole,
            GetRole,
            ListRoles,
            DeleteRole,
            GrantRole,
            RevokeRole,
            ListGrants,
            ListPermissions,
            CheckPermission,
            GetMetrics
        };
    }

    static Action actionFromString(const std::string &action) {
        if (action == "login") return Action::Login;
        if (action == "refresh-session") return Action::RefreshSession;
        if (action == "oidc-authorize") return Action::OidcAuthorize;
        // "oidc-callback" is the same action under the name the provider's redirect arrives at -
        // see the gateway's /eam/oidc/callback route.
        if (action == "oidc-login" || action == "oidc-callback") return Action::OidcLogin;
        if (action == "saml-authorize" || action == "saml-login") return Action::SamlAuthorize;
        if (action == "saml-acs") return Action::SamlAcs;
        if (action == "saml-metadata") return Action::SamlMetadata;
        if (action == "register") return Action::Register;
        if (action == "get-user") return Action::GetUser;
        if (action == "get-user-group") return Action::GetUserGroup;
        if (action == "list-users") return Action::ListUsers;
        if (action == "delete-user") return Action::DeleteUser;
        if (action == "change-password") return Action::ChangePassword;
        if (action == "change-userid") return Action::ChangeUserId;
        if (action == "create-access-key") return Action::CreateAccessKey;
        if (action == "list-access-keys") return Action::ListAccessKeys;
        if (action == "delete-access-key") return Action::DeleteAccessKey;
        if (action == "create-user-group") return Action::CreateUserGroup;
        if (action == "list-user-groups") return Action::ListUserGroups;
        if (action == "user-group-add-user") return Action::UserGroupAddUser;
        if (action == "user-group-remove-user") return Action::UserGroupRemoveUser;
        if (action == "delete-user-group") return Action::DeleteUserGroup;
        if (action == "create-account") return Action::CreateAccount;
        if (action == "get-account") return Action::GetAccount;
        if (action == "list-accounts") return Action::ListAccounts;
        if (action == "delete-account") return Action::DeleteAccount;
        if (action == "create-namespace") return Action::CreateNamespace;
        if (action == "list-namespaces") return Action::ListNamespaces;
        if (action == "delete-namespace") return Action::DeleteNamespace;
        if (action == "change-namespace") return Action::ChangeNamespace;
        if (action == "create-role") return Action::CreateRole;
        if (action == "update-role") return Action::UpdateRole;
        if (action == "get-role") return Action::GetRole;
        if (action == "list-roles") return Action::ListRoles;
        if (action == "delete-role") return Action::DeleteRole;
        if (action == "grant-role") return Action::GrantRole;
        if (action == "revoke-role") return Action::RevokeRole;
        if (action == "list-grants") return Action::ListGrants;
        if (action == "list-permissions") return Action::ListPermissions;
        if (action == "check-permission") return Action::CheckPermission;
        if (action == "get-metrics") return Action::GetMetrics;
        return Action::Unknown;
    }

    static response<string_body> dispatch(const request<string_body> &req) {

        const auto action = std::string(req["x-euclid-action"]);
        if (action.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request, "Missing x-euclid-action header");
        }
        log_debug << "EAM action=" << action;

        switch (actionFromString(action)) {

            case Action::Login:
                return handleLogin(req);

            case Action::RefreshSession:
                return handleRefreshSession(req);

            case Action::OidcAuthorize:
                return handleOidcAuthorize(req);

            case Action::OidcLogin:
                return handleOidcLogin(req);

            case Action::SamlAuthorize:
                return handleSamlAuthorize(req);

            case Action::SamlAcs:
                return handleSamlAcs(req);

            case Action::SamlMetadata:
                return handleSamlMetadata(req);

            case Action::Register:
                return handleRegister(req);

            case Action::ListUsers:
                return handleListUsers(req);

            case Action::DeleteUser:
                return handleDeleteUser(req);

            case Action::ChangePassword:
                return handleChangePassword(req);

            case Action::ChangeUserId:
                return handleChangeUserId(req);

            case Action::CreateAccessKey:
                return handleCreateAccessKey(req);

            case Action::ListAccessKeys:
                return handleListAccessKeys(req);

            case Action::DeleteAccessKey:
                return handleDeleteAccessKey(req);

            case Action::CreateUserGroup:
                return handleCreateUserGroup(req);

            case Action::GetUser:
                return handleGetUser(req);

            case Action::GetUserGroup:
                return handleGetUserGroup(req);

            case Action::ListUserGroups:
                return handleListUserGroups(req);

            case Action::UserGroupAddUser:
                return handleUserGroupAddUser(req);

            case Action::UserGroupRemoveUser:
                return handleUserGroupRemoveUser(req);

            case Action::DeleteUserGroup:
                return handleDeleteUserGroup(req);

            case Action::CreateAccount:
                return handleCreateAccount(req);

            case Action::GetAccount:
                return handleGetAccount(req);

            case Action::ListAccounts:
                return handleListAccounts(req);

            case Action::DeleteAccount:
                return handleDeleteAccount(req);

            case Action::CreateNamespace:
                return handleCreateNamespace(req);

            case Action::ListNamespaces:
                return handleListNamespaces(req);

            case Action::DeleteNamespace:
                return handleDeleteNamespace(req);



            case Action::ChangeNamespace:
                return handleChangeNamespace(req);

            case Action::CreateRole:
                return handleCreateRole(req);
            case Action::UpdateRole:
                return handleUpdateRole(req);
            case Action::GetRole:
                return handleGetRole(req);
            case Action::ListRoles:
                return handleListRoles(req);
            case Action::DeleteRole:
                return handleDeleteRole(req);
            case Action::GrantRole:
                return handleGrantRole(req);
            case Action::RevokeRole:
                return handleRevokeRole(req);
            case Action::ListGrants:
                return handleListGrants(req);
            case Action::ListPermissions:
                return handleListPermissions(req);
            case Action::CheckPermission:
                return handleCheckPermission(req);
            case Action::GetMetrics:
                return EamServer::MetricsResponse(req);

            case Action::Unknown:
            default:
                return EamServer::ErrorResponse(req, status::not_found, "Action not implemented: " + action);
        }
    }

    // ── EamServer ─────────────────────────────────────────────────────────

    EamServer::EamServer(std::string socketPath, const int threads) : HttpActionServer("EAM", std::move(socketPath), threads) {
        auto &scheduler = Core::Scheduler::instance();
        scheduler.Start();
        _currentUsersTaskId = scheduler.SchedulePeriodic("access-report-current-users", [] { reportCurrentUsers(); }, std::chrono::seconds(15));
    }

    EamServer::~EamServer() {
        Core::Scheduler::instance().Cancel(_currentUsersTaskId);
    }

    response<string_body> EamServer::DispatchAction(const request<string_body> &req) {
        return dispatch(req);
    }

}// namespace Euclid::EAM