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
#include <euclid/dto/eam/CreateAccountRequest.h>
#include <euclid/dto/eam/CreateAccountResponse.h>
#include <euclid/dto/eam/CreateNamespaceRequest.h>
#include <euclid/dto/eam/CreateNamespaceResponse.h>
#include <euclid/dto/eam/DeleteAccountRequest.h>
#include <euclid/dto/eam/DeleteNamespaceRequest.h>
#include <euclid/dto/eam/DeleteUserGroupRequest.h>
#include <euclid/dto/eam/GrantNamespaceAccessRequest.h>
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
#include <euclid/dto/eam/RevokeNamespaceAccessRequest.h>
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

    // Whether user may administer accountId's namespaces/grants: either a global administrator,
    // or holds an account-scoped AccountGrant.isAdmin for that specific account. Account creation
    // and deletion themselves stay global-admin-only (see handleCreateAccount/handleDeleteAccount)
    // since those are platform-level operations, not delegable to an account owner.
    static bool isAccountAdmin(const Database::Entity::EAM::User &user, const std::string &accountId) {
        if (isAdmin(user)) return true;
        return std::ranges::any_of(user.accountGrants, [&](const auto &grant) { return grant.accountId == accountId && grant.isAdmin; });
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

    }// namespace

    // What an installation is willing to do with somebody its identity provider has vouched for.
    // The two federations answer this from their own configuration blocks, and the answer means
    // the same thing in both.
    struct ProvisioningPolicy {
        bool jitProvisioning{true};
        bool linkExistingUsers{false};
        std::string accountId;
    };

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

        // No index on accountGrants.accountId - acceptable given this is an infrequent,
        // admin-only operation over an expected-small user set.
        const auto users = repo->listUsers("", 0, 0, "");
        const bool stillGranted = std::ranges::any_of(users, [&](const auto &user) {
            return std::ranges::any_of(user.accountGrants, [&](const auto &grant) { return grant.accountId == request.accountId; });
        });
        if (stillGranted) {
            return EamServer::ErrorResponse(req, status::conflict, "Account still has user grants, revoke those first");
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

        const auto users = repo->listUsers("", 0, 0, "");
        const bool stillGranted = std::ranges::any_of(users, [&](const auto &user) {
            return std::ranges::any_of(user.accountGrants, [&](const auto &grant) {
                return grant.accountId == request.accountId && std::ranges::contains(grant.namespaces, request.name);
            });
        });
        if (stillGranted) {
            return EamServer::ErrorResponse(req, status::conflict, "Namespace still has user grants, revoke those first");
        }

        repo->deleteNamespace(request.accountId, request.name);
        log_info << "Namespace deleted, accountId: " << request.accountId << ", name: " << request.name;

        return EamServer::JsonResponse(req, status::ok);
    }

    static response<string_body> handleGrantNamespaceAccess(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "grant-namespace-access");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) {
            return unauthorized(req, auth);
        }

        boost::json::value jv;
        if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EAM::GrantNamespaceAccessRequest>(jv);
        if (request.user.empty() || request.accountId.empty() || request.ns.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request, "user, accountId and namespace are required");
        }
        if (!isAccountAdmin(*auth.user, request.accountId)) {
            return EamServer::ErrorResponse(req, status::forbidden, "Administrator privileges required for this account");
        }

        const auto repo = Database::RepositoryFactory::instance().eamRepository();
        if (!repo->namespaceExists(request.accountId, request.ns)) {
            return EamServer::ErrorResponse(req, status::conflict, "Namespace does not exist");
        }
        if (!repo->userErnExists(request.user)) {
            return EamServer::ErrorResponse(req, status::conflict, "User does not exist");
        }

        auto user = repo->findUserByErn(request.user);
        const auto grantIt = std::ranges::find_if(user->accountGrants, [&](const auto &g) { return g.accountId == request.accountId; });
        if (grantIt == user->accountGrants.end()) {
            user->accountGrants.push_back({.accountId = request.accountId, .namespaces = {request.ns}, .granted = Core::DateTimeUtils::ToISO8601(std::chrono::system_clock::now())});
        } else if (std::ranges::contains(grantIt->namespaces, request.ns)) {
            return EamServer::ErrorResponse(req, status::conflict, "Grant already exists");
        } else {
            grantIt->namespaces.push_back(request.ns);
        }

        const auto saved = repo->upsertUser(*user);
        log_info << "Namespace access granted, user: " << saved.userId << ", accountId: " << request.accountId << ", namespace: " << request.ns;

        return EamServer::JsonResponse(req, status::ok);
    }

    static response<string_body> handleRevokeNamespaceAccess(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "revoke-namespace-access");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) {
            return unauthorized(req, auth);
        }

        boost::json::value jv;
        if (const auto err = EamServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EAM::RevokeNamespaceAccessRequest>(jv);
        if (request.user.empty() || request.accountId.empty() || request.ns.empty()) {
            return EamServer::ErrorResponse(req, status::bad_request, "user, accountId and namespace are required");
        }
        if (!isAccountAdmin(*auth.user, request.accountId)) {
            return EamServer::ErrorResponse(req, status::forbidden, "Administrator privileges required for this account");
        }

        const auto repo = Database::RepositoryFactory::instance().eamRepository();
        if (!repo->userErnExists(request.user)) {
            return EamServer::ErrorResponse(req, status::conflict, "User does not exist");
        }

        auto user = repo->findUserByErn(request.user);
        const auto grantIt = std::ranges::find_if(user->accountGrants, [&](const auto &g) { return g.accountId == request.accountId; });
        if (grantIt == user->accountGrants.end() || !std::ranges::contains(grantIt->namespaces, request.ns)) {
            return EamServer::ErrorResponse(req, status::conflict, "Grant does not exist");
        }
        std::erase(grantIt->namespaces, request.ns);
        if (grantIt->namespaces.empty()) {
            std::erase_if(user->accountGrants, [&](const auto &g) { return g.accountId == request.accountId; });
        }

        const auto saved = repo->upsertUser(*user);
        log_info << "Namespace access revoked, user: " << saved.userId << ", accountId: " << request.accountId << ", namespace: " << request.ns;

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
                             std::ranges::any_of(auth.user->accountGrants, [&](const auto &grant) {
                                 return grant.accountId == auth.user->accountId && std::ranges::contains(grant.namespaces, request.ns);
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
        enum class Action {
            Unknown,
            Login,
            OidcAuthorize,
            OidcLogin,
            SamlAuthorize,
            SamlAcs,
            SamlMetadata,
            Register,
            ListUsers,
            DeleteUser,
            CreateAccessKey,
            ListAccessKeys,
            DeleteAccessKey,
            CreateUserGroup,
            ListUserGroups,
            UserGroupAddUser,
            UserGroupRemoveUser,
            DeleteUserGroup,
            CreateAccount,
            ListAccounts,
            DeleteAccount,
            CreateNamespace,
            ListNamespaces,
            DeleteNamespace,
            GrantNamespaceAccess,
            RevokeNamespaceAccess,
            ChangeNamespace,
            GetMetrics
        };
    }

    static Action actionFromString(const std::string &action) {
        if (action == "login") return Action::Login;
        if (action == "oidc-authorize") return Action::OidcAuthorize;
        // "oidc-callback" is the same action under the name the provider's redirect arrives at -
        // see the gateway's /eam/oidc/callback route.
        if (action == "oidc-login" || action == "oidc-callback") return Action::OidcLogin;
        if (action == "saml-authorize" || action == "saml-login") return Action::SamlAuthorize;
        if (action == "saml-acs") return Action::SamlAcs;
        if (action == "saml-metadata") return Action::SamlMetadata;
        if (action == "register") return Action::Register;
        if (action == "list-users") return Action::ListUsers;
        if (action == "delete-user") return Action::DeleteUser;
        if (action == "create-access-key") return Action::CreateAccessKey;
        if (action == "list-access-keys") return Action::ListAccessKeys;
        if (action == "delete-access-key") return Action::DeleteAccessKey;
        if (action == "create-user-group") return Action::CreateUserGroup;
        if (action == "list-user-groups") return Action::ListUserGroups;
        if (action == "user-group-add-user") return Action::UserGroupAddUser;
        if (action == "user-group-remove-user") return Action::UserGroupRemoveUser;
        if (action == "delete-user-group") return Action::DeleteUserGroup;
        if (action == "create-account") return Action::CreateAccount;
        if (action == "list-accounts") return Action::ListAccounts;
        if (action == "delete-account") return Action::DeleteAccount;
        if (action == "create-namespace") return Action::CreateNamespace;
        if (action == "list-namespaces") return Action::ListNamespaces;
        if (action == "delete-namespace") return Action::DeleteNamespace;
        if (action == "grant-namespace-access") return Action::GrantNamespaceAccess;
        if (action == "revoke-namespace-access") return Action::RevokeNamespaceAccess;
        if (action == "change-namespace") return Action::ChangeNamespace;
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

            case Action::CreateAccessKey:
                return handleCreateAccessKey(req);

            case Action::ListAccessKeys:
                return handleListAccessKeys(req);

            case Action::DeleteAccessKey:
                return handleDeleteAccessKey(req);

            case Action::CreateUserGroup:
                return handleCreateUserGroup(req);

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

            case Action::GrantNamespaceAccess:
                return handleGrantNamespaceAccess(req);

            case Action::RevokeNamespaceAccess:
                return handleRevokeNamespaceAccess(req);

            case Action::ChangeNamespace:
                return handleChangeNamespace(req);

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

    response<string_body> EamServer::Dispatch(const request<string_body> &req) {
        return dispatch(req);
    }

}// namespace Euclid::EAM