// C++ includes
#include <algorithm>

// Euclid includes
#include <EamServer.h>
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
        // to sign with and deliberately no way in through here. Checked after the password so that
        // a caller cannot tell the two kinds of account apart by how quickly they are refused.
        if (!user->loginEnabled) {
            log_warning << "Login refused for technical user, userId: " << user->userId;
            return {};
        }

        log_info << "Login succeeded, userId: " << user->userId;

        auto updatedUser = *user;

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
            log_info << "Access key provisioned on login, userId: " << user->userId << ", accessKeyId: " << key.accessKeyId;
        }

        Database::Entity::EAM::Session session;
        session.createdAt = Core::DateTimeUtils::ToISO8601(Core::DateTimeUtils::UtcDateTimeNow());
        session.expiresAt = Core::DateTimeUtils::ToISO8601(Core::DateTimeUtils::UtcDateTimeNow() + kSessionTtl);
        updatedUser.sessions.push_back(session);

        // Always persisted (not just when a new access key was minted) - otherwise a login that
        // reuses an existing key would never record its session, and "current users" would only
        // ever count first-time logins.
        repo->upsertUser(updatedUser);

        Dto::EAM::LoginResponse response;
        response.token = Core::JwtUtils::CreateToken(user->userId, EamServer::JwtSecret(), kSessionTtl);
        response.user = user->userId;
        response.accountId = user->accountId;
        response.region = user->region;
        response.accessKeyId = key.accessKeyId;
        response.secretAccessKey = key.secretAccessKey;
        response.createdAt = key.created;
        response.isAdmin = isAdmin(*user);
        return response;
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

        const std::vector<Database::Entity::EAM::User> users = repo->listUsers(request.prefix, request.pageSize, request.pageIndex, request.sortColumn);
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

        const std::vector<Database::Entity::EAM::UserGroup> userGroups = repo->listUserGroups(request.prefix, request.pageSize, request.pageIndex, request.sortColumn);
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

        const auto accounts = repo->listAccounts(request.prefix, request.pageSize, request.pageIndex, request.sortColumn);
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
        const auto namespaces = repo->listNamespaces(request.accountId, request.prefix, request.pageSize, request.pageIndex, request.sortColumn);
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