// C++ includes
#include <algorithm>

// Euclid includes
#include <euclid/core/Scheduler.h>
#include <euclid/core/monitoring/MetricEventBus.h>
#include <euclid/core/monitoring/MonitoringTimer.h>
#include <AccessServer.h>

namespace Euclid::Access {

    namespace beast = boost::beast;
    namespace http = beast::http;

    // ── Helpers ──────────────────────────────────────────────────────────────

    namespace {
        // Looks up the caller identity resolved by AccessServer::Authenticate(), by user ID.
        // Distinguishing an expired token lets handlers return a more specific error than a plain 401.
        struct AuthResult {
            std::optional<Database::Entity::Access::User> user;
            bool tokenExpired{false};
            std::string denialReason;
        };
    }// namespace

    static AuthResult authenticate(const request<string_body> &req) {
        const auto auth = AccessServer::Authenticate(req);
        if (!auth.subject.has_value()) {
            return {.user = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason};
        }
        return {.user = Database::RepositoryFactory::instance().accessRepository()->findUserByUserId(*auth.subject)};
    }

    static response<string_body> unauthorized(const request<string_body> &req, const AuthResult &auth) {
        return AccessServer::Unauthorized(req, {.subject = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason});
    }

    // Token lifetime, and the matching lifetime given to the Session record created on login -
    // named so the two can't silently drift apart.
    constexpr auto kSessionTtl = std::chrono::hours(1);

    // Timer/counter names shared by every handler below - one series per action, labeled
    // "method"=<action>.
    constexpr auto kServiceTimer = "access-service-time";
    constexpr auto kServiceCounter = "access-service-count";

    // Counts users with at least one non-expired session. Fired as a gauge (see
    // reportCurrentUsers()) rather than derived per-request, since it's shared state - every
    // access instance in an autoscaled pool would otherwise report/poll it independently.
    static long countCurrentUsers() {
        const auto repo = Database::RepositoryFactory::instance().accessRepository();
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
        Core::Monitoring::MetricEventBus::instance().sigMetricGauge("access-current-users", "", "", static_cast<double>(countCurrentUsers()));
    }

    // Looks up the user by userId, falling back to email, and checks the password
    // against the stored PBKDF2-HMAC-SHA256 hash.
    static Dto::Access::LoginResponse doLogin(const Dto::Access::LoginRequest &request) {

        const auto repo = Database::RepositoryFactory::instance().accessRepository();

        const std::optional<Database::Entity::Access::User> user = !request.userId.empty()
                                                                       ? repo->findUserByUserId(request.userId)
                                                                       : repo->findUserByEmail(request.email);

        if (!user.has_value() || !Core::PasswordUtils::Verify(request.password, user->password)) {
            log_warning << "Login failed, userId: " << request.userId << ", email: " << request.email;
            return {};
        }

        log_info << "Login succeeded, userId: " << user->userId;

        auto updatedUser = *user;

        // Reuses the caller's existing active access key if there is one, so login stays
        // self-sufficient for SigV4-signed calls without minting a new key (and invalidating
        // local credentials elsewhere) on every login.
        const Database::Entity::Access::AccessKey *existingKey = nullptr;
        for (const auto &candidate: updatedUser.accessKeys) {
            if (candidate.active) {
                existingKey = &candidate;
                break;
            }
        }

        Database::Entity::Access::AccessKey key;
        if (existingKey != nullptr) {
            key = *existingKey;
        } else {
            key.accessKeyId = Core::CryptoUtils::GenerateAccessKeyId();
            key.secretAccessKey = Core::CryptoUtils::GenerateSecretAccessKey();
            key.active = true;
            key.createdAt = Core::DateTimeUtils::ToISO8601(Core::DateTimeUtils::UtcDateTimeNow());
            updatedUser.accessKeys.push_back(key);
            log_info << "Access key provisioned on login, userId: " << user->userId << ", accessKeyId: " << key.accessKeyId;
        }

        Database::Entity::Access::Session session;
        session.createdAt = Core::DateTimeUtils::ToISO8601(Core::DateTimeUtils::UtcDateTimeNow());
        session.expiresAt = Core::DateTimeUtils::ToISO8601(Core::DateTimeUtils::UtcDateTimeNow() + kSessionTtl);
        updatedUser.sessions.push_back(session);

        // Always persisted (not just when a new access key was minted) - otherwise a login that
        // reuses an existing key would never record its session, and "current users" would only
        // ever count first-time logins.
        repo->upsertUser(updatedUser);

        Dto::Access::LoginResponse response;
        response.token = Core::JwtUtils::CreateToken(user->userId, AccessServer::JwtSecret(), kSessionTtl);
        response.user = user->userId;
        response.accountId = user->accountId;
        response.region = user->region;
        response.accessKeyId = key.accessKeyId;
        response.secretAccessKey = key.secretAccessKey;
        response.createdAt = key.createdAt;
        return response;
    }

    static response<string_body> handleLogin(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "login");

        boost::json::value jv;
        if (const auto err = AccessServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::Access::LoginRequest>(jv);
        const auto response = doLogin(request);

        if (response.token.empty()) {
            return AccessServer::ErrorResponse(req, status::unauthorized, "Invalid credentials");
        }

        return AccessServer::JsonResponse(req, status::ok, response.toJson());
    }

    // Registers a new user. Requires the caller to be an administrator, with one exception:
    // when the user store is completely empty, the request is allowed through unauthenticated
    // and the new user is force-promoted to administrator, so there's always a way to bootstrap
    // the first account.
    static response<string_body> handleRegister(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "register");

        const auto repo = Database::RepositoryFactory::instance().accessRepository();
        const bool bootstrap = repo->countUsers() <= 0;

        if (!bootstrap) {
            const auto auth = authenticate(req);
            if (!auth.user.has_value()) {
                return unauthorized(req, auth);
            }
            if (!auth.user->isAdmin) {
                return AccessServer::ErrorResponse(req, status::forbidden, "Administrator privileges required");
            }
        }

        boost::json::value jv;
        if (const auto err = AccessServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::Access::RegisterRequest>(jv);
        if (request.userId.empty() || request.password.empty()) {
            return AccessServer::ErrorResponse(req, status::bad_request, "userId and password are required");
        }

        if (repo->userExists(request.userId)) {
            return AccessServer::ErrorResponse(req, status::conflict, "User already exists");
        }

        Database::Entity::Access::User user;
        user.userId = request.userId;
        user.email = request.email;
        user.accountId = request.accountId;
        user.region = request.region;
        user.password = Core::PasswordUtils::Hash(request.password);
        user.isAdmin = bootstrap || request.isAdmin;

        const auto saved = repo->upsertUser(user);
        log_info << "User registered, userId: " << saved.userId << ", isAdmin: " << std::boolalpha << saved.isAdmin << (bootstrap ? " (bootstrap)" : "");

        Dto::Access::RegisterResponse response;
        response.user.userId = saved.userId;
        response.user.email = saved.email;
        response.user.accountId = saved.accountId;
        response.user.region = saved.region;
        return AccessServer::JsonResponse(req, status::created, response.toJson());
    }

    // Lists users. Requires the caller to be an authenticated administrator - unlike
    // handleRegister() there's no bootstrap exception, since an empty store has nothing to list.
    static response<string_body> handleListUsers(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-users");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) {
            return unauthorized(req, auth);
        }
        if (!auth.user->isAdmin) {
            return AccessServer::ErrorResponse(req, status::forbidden, "Administrator privileges required");
        }

        const auto repo = Database::RepositoryFactory::instance().accessRepository();

        boost::json::value jv;
        if (const auto err = AccessServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::Access::ListUserRequest>(jv);

        const std::vector<Database::Entity::Access::User> users = repo->listUsers(request.prefix, request.pageSize, request.pageIndex, request.sortColumn);
        log_info << "Access ListUsers" << (!request.prefix.empty() ? ", prefix: " + request.prefix : "");

        Dto::Access::ListUserResponse response;
        response.users = Dto::Access::AccessMapper::toDto(users);
        response.total = repo->countUsers();
        return AccessServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::value_from(response)));
    }

    // Deletes a user. Requires the caller to be an authenticated administrator - unlike
    // handleRegister() there's no bootstrap exception, since deleting requires an existing admin.
    static response<string_body> handleDeleteUser(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-user");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) {
            return unauthorized(req, auth);
        }
        if (!auth.user->isAdmin) {
            return AccessServer::ErrorResponse(req, status::forbidden, "Administrator privileges required");
        }

        const auto repo = Database::RepositoryFactory::instance().accessRepository();

        boost::json::value jv;
        if (const auto err = AccessServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::Access::DeleteUserRequest>(jv);
        if (request.userId.empty()) {
            return AccessServer::ErrorResponse(req, status::bad_request, "userId is required");
        }

        if (!repo->userExists(request.userId)) {
            return AccessServer::ErrorResponse(req, status::not_found, "User not found");
        }

        repo->deleteUser(request.userId);
        log_info << "User deleted, userId: " << request.userId;

        return AccessServer::JsonResponse(req, status::ok);
    }

    // Creates a new access key for the authenticated caller. Self-service - unlike
    // handleListUsers/handleDeleteUser there's no admin requirement, since a user managing
    // their own signing credentials doesn't need elevated privileges (mirrors AWS IAM).
    static response<string_body> handleCreateAccessKey(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "create-access-key");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) {
            return unauthorized(req, auth);
        }

        Database::Entity::Access::AccessKey key;
        key.accessKeyId = Core::CryptoUtils::GenerateAccessKeyId();
        key.secretAccessKey = Core::CryptoUtils::GenerateSecretAccessKey();
        key.active = true;
        key.createdAt = Core::DateTimeUtils::ToISO8601(Core::DateTimeUtils::UtcDateTimeNow());

        auto user = *auth.user;
        user.accessKeys.push_back(key);
        Database::RepositoryFactory::instance().accessRepository()->upsertUser(user);
        log_info << "Access key created, userId: " << user.userId << ", accessKeyId: " << key.accessKeyId;

        Dto::Access::CreateAccessKeyResponse response;
        response.accessKeyId = key.accessKeyId;
        response.secretAccessKey = key.secretAccessKey;
        response.createdAt = key.createdAt;
        return AccessServer::JsonResponse(req, status::created, response.toJson());
    }

    // Lists the authenticated caller's own access keys (never including the secret).
    static response<string_body> handleListAccessKeys(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-access-keys");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) {
            return unauthorized(req, auth);
        }

        Dto::Access::ListAccessKeysResponse response;
        for (const auto &key: auth.user->accessKeys) {
            response.accessKeys.push_back({.accessKeyId = key.accessKeyId, .active = key.active, .createdAt = key.createdAt});
        }
        return AccessServer::JsonResponse(req, status::ok, response.toJson());
    }

    // Deletes one of the authenticated caller's own access keys.
    static response<string_body> handleDeleteAccessKey(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-access-key");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) {
            return unauthorized(req, auth);
        }

        boost::json::value jv;
        if (const auto err = AccessServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::Access::DeleteAccessKeyRequest>(jv);
        if (request.accessKeyId.empty()) {
            return AccessServer::ErrorResponse(req, status::bad_request, "accessKeyId is required");
        }

        auto user = *auth.user;
        const auto before = user.accessKeys.size();
        std::erase_if(user.accessKeys, [&](const auto &key) { return key.accessKeyId == request.accessKeyId; });
        if (user.accessKeys.size() == before) {
            return AccessServer::ErrorResponse(req, status::not_found, "Access key not found");
        }

        Database::RepositoryFactory::instance().accessRepository()->upsertUser(user);
        log_info << "Access key deleted, userId: " << user.userId << ", accessKeyId: " << request.accessKeyId;

        return AccessServer::JsonResponse(req, status::ok);
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
        if (action == "get-metrics") return Action::GetMetrics;
        return Action::Unknown;
    }

    static response<string_body> dispatch(const request<string_body> &req) {

        const auto action = std::string(req["x-euclid-action"]);
        if (action.empty()) {
            return AccessServer::ErrorResponse(req, status::bad_request, "Missing x-euclid-action header");
        }
        log_debug << "Access action=" << action;

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

            case Action::GetMetrics:
                return AccessServer::MetricsResponse(req);

            case Action::Unknown:
            default:
                return AccessServer::ErrorResponse(req, status::not_found, "Action not implemented: " + action);
        }
    }

    // ── AccessServer ─────────────────────────────────────────────────────────

    AccessServer::AccessServer(std::string socketPath, const int threads) : HttpActionServer("Access", std::move(socketPath), threads) {
        auto &scheduler = Core::Scheduler::instance();
        scheduler.Start();
        _currentUsersTaskId = scheduler.SchedulePeriodic("access-report-current-users", [] { reportCurrentUsers(); }, std::chrono::seconds(15));
    }

    AccessServer::~AccessServer() {
        Core::Scheduler::instance().Cancel(_currentUsersTaskId);
    }

    response<string_body> AccessServer::Dispatch(const request<string_body> &req) {
        return dispatch(req);
    }

}// namespace Euclid::Access