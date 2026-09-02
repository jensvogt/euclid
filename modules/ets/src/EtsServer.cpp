// C++ includes
#include <optional>
#include <string>
#include <vector>

// Euclid includes
#include <EtsServer.h>
#include <euclid/core/DateTimeUtils.h>
#include <euclid/core/ErnUtils.h>
#include <euclid/core/monitoring/MonitoringTimer.h>
#include <euclid/database/entity/eam/User.h>

namespace Euclid::ETS {

    namespace {
        constexpr auto kServiceTimer = "ets-service-time";
        constexpr auto kServiceCounter = "ets-service-count";
    }// namespace

    using Database::Entity::ETS::TransferProtocol;
    using Database::Entity::ETS::TransferProtocolFromString;
    using Database::Entity::ETS::TransferProtocolToString;
    using Database::Entity::ETS::TransferServer;
    using Database::Entity::ETS::TransferServerState;
    using Database::Entity::ETS::TransferServerStateToString;

    // ── Helpers ──────────────────────────────────────────────────────────────

    namespace {

        struct AuthResult {
            std::optional<Database::Entity::EAM::User> user;
            bool tokenExpired{false};
            std::string denialReason;
        };

        std::vector<std::string> stringArray(const boost::json::object &obj, const std::string &key) {
            std::vector<std::string> result;
            if (const auto *v = obj.if_contains(key); v && v->is_array()) {
                for (const auto &element: v->as_array()) {
                    if (element.is_string()) result.emplace_back(element.as_string().c_str());
                }
            }
            return result;
        }

        std::string stringField(const boost::json::object &obj, const std::string &key, const std::string &fallback = "") {
            if (const auto *v = obj.if_contains(key); v && v->is_string()) return v->as_string().c_str();
            return fallback;
        }

        long longField(const boost::json::object &obj, const std::string &key, const long fallback = 0) {
            if (const auto *v = obj.if_contains(key); v && v->is_number()) return v->to_number<long>();
            return fallback;
        }

        // The manager publishes each module's live instances, so a server's *observed* state is
        // read from there rather than stored - the definition only ever carries what it should be.
        std::string observedState(const std::string &serverId) {
            for (const auto &module: Database::RepositoryFactory::instance().emmRepository()->findAll()) {
                if (module.name != serverId) continue;
                for (const auto &instance: module.instances) {
                    if (instance.state == Database::Entity::ModuleState::RUNNING) return "RUNNING";
                }
                return "STOPPED";
            }
            return "STOPPED";
        }

        boost::json::object toJson(const TransferServer &server) {

            boost::json::array userIds;
            for (const auto &userId: server.userIds) userIds.push_back(boost::json::string(userId));

            boost::json::array userGroups;
            for (const auto &group: server.userGroups) userGroups.push_back(boost::json::string(group));

            return boost::json::object{
                    {"serverId", server.serverId},
                    {"ern", server.ern},
                    {"accountId", server.accountId},
                    {"region", server.region},
                    {"protocol", TransferProtocolToString(server.protocol)},
                    {"address", server.address},
                    {"port", server.port},
                    {"bucketName", server.bucketName},
                    {"bucketErn", server.bucketErn},
                    {"homeDirectory", server.homeDirectory},
                    {"userIds", userIds},
                    {"userGroups", userGroups},
                    {"desiredState", TransferServerStateToString(server.desiredState)},
                    {"state", observedState(server.serverId)},
                    {"hostKey", server.hostKey},
                    {"pasvMin", server.pasvMin},
                    {"pasvMax", server.pasvMax},
                    {"created", Core::DateTimeUtils::ToISO8601(server.created)},
                    {"modified", Core::DateTimeUtils::ToISO8601(server.modified)}
            };
        }

    }// namespace

    static AuthResult authenticate(const request<string_body> &req) {
        const auto auth = EtsServer::Authenticate(req);
        if (!auth.subject.has_value()) {
            return {.user = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason};
        }
        return {.user = Database::RepositoryFactory::instance().eamRepository()->findUserByUserId(*auth.subject)};
    }

    static response<string_body> unauthorized(const request<string_body> &req, const AuthResult &auth) {
        return EtsServer::Unauthorized(req, {.subject = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason});
    }

    // Every action here changes (or reveals) which network listeners exist and who may reach
    // which bucket through them, so all of them are administrator-only.
    static std::optional<response<string_body> > requireAdmin(const request<string_body> &req, AuthResult &auth) {
        auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);
        if (!Database::IsEamAdmin(*Database::RepositoryFactory::instance().eamRepository(), auth.user->userId)) {
            return EtsServer::ErrorResponse(req, status::forbidden, "Administrator privileges required");
        }
        return std::nullopt;
    }

    // ── Action handlers ──────────────────────────────────────────────────────

    static response<string_body> handleCreateServer(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "create-server");

        AuthResult auth;
        if (const auto denied = requireAdmin(req, auth)) return *denied;

        boost::json::value jv;
        if (const auto err = EtsServer::ParseJsonBody(req, jv)) return *err;
        if (!jv.is_object()) return EtsServer::ErrorResponse(req, status::bad_request, "Expected a JSON object body");
        const auto &obj = jv.as_object();

        const auto serverId = stringField(obj, "serverId");
        if (serverId.empty()) return EtsServer::ErrorResponse(req, status::bad_request, "serverId is required");

        const auto repo = Database::RepositoryFactory::instance().etsRepository();
        if (repo->serverExists(serverId)) {
            return EtsServer::ErrorResponse(req, status::conflict, "Transfer server already exists: " + serverId);
        }

        const auto protocol = TransferProtocolFromString(stringField(obj, "protocol", "SFTP"));
        if (protocol == TransferProtocol::UNKNOWN) {
            return EtsServer::ErrorResponse(req, status::bad_request, "protocol must be FTP or SFTP");
        }

        const auto port = longField(obj, "port");
        if (port <= 0 || port > 65535) {
            return EtsServer::ErrorResponse(req, status::bad_request, "port must be between 1 and 65535");
        }

        // Two servers on one port would leave whichever started second permanently failing to
        // bind, with nothing in the definition to explain why.
        for (const auto &existing: repo->listServers("")) {
            if (existing.port == port) {
                return EtsServer::ErrorResponse(req, status::conflict,
                                                "port " + std::to_string(port) + " is already used by transfer server '" + existing.serverId + "'");
            }
        }

        // The bucket is resolved now rather than at start-up, so a typo is reported to whoever
        // creates the server instead of surfacing later as a server that runs but stores nothing.
        const auto bucketName = stringField(obj, "bucket");
        if (bucketName.empty()) return EtsServer::ErrorResponse(req, status::bad_request, "bucket is required");

        const auto bucket = Database::RepositoryFactory::instance().esmRepository()->findBucketByName(bucketName);
        if (!bucket.has_value()) {
            return EtsServer::ErrorResponse(req, status::not_found, "Bucket not found: " + bucketName);
        }

        TransferServer server;
        server.serverId = serverId;
        server.accountId = auth.user->accountId;
        server.region = auth.user->region;
        server.ern = Core::createEtsServerErn(server.accountId, serverId);
        server.protocol = protocol;
        server.address = stringField(obj, "address", "0.0.0.0");
        server.port = port;
        server.bucketName = bucket->name;
        server.bucketErn = bucket->ern;
        server.homeDirectory = stringField(obj, "homeDirectory");
        server.userIds = stringArray(obj, "userIds");
        server.userGroups = stringArray(obj, "userGroups");
        server.desiredState = TransferServerState::STOPPED;
        server.hostKey = stringField(obj, "hostKey");
        server.pasvMin = longField(obj, "pasvMin", 6000);
        server.pasvMax = longField(obj, "pasvMax", 6100);

        const auto stored = repo->upsertServer(server);
        log_info << "ETS created transfer server, serverId: " << stored.serverId << ", protocol: " << TransferProtocolToString(stored.protocol)
                << ", port: " << stored.port << ", bucket: " << stored.bucketName << ", homeDirectory: " << stored.homeDirectory;

        return EtsServer::JsonResponse(req, status::ok, boost::json::serialize(toJson(stored)));
    }

    static response<string_body> handleUpdateServer(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "update-server");

        AuthResult auth;
        if (const auto denied = requireAdmin(req, auth)) return *denied;

        boost::json::value jv;
        if (const auto err = EtsServer::ParseJsonBody(req, jv)) return *err;
        if (!jv.is_object()) return EtsServer::ErrorResponse(req, status::bad_request, "Expected a JSON object body");
        const auto &obj = jv.as_object();

        const auto serverId = stringField(obj, "serverId");
        const auto repo = Database::RepositoryFactory::instance().etsRepository();
        auto server = repo->findServerByServerId(serverId);
        if (!server.has_value()) {
            return EtsServer::ErrorResponse(req, status::not_found, "Transfer server not found: " + serverId);
        }

        // Only the fields actually present in the body are touched, so a caller can flip one
        // setting without having to resend the whole definition.
        if (obj.contains("address")) server->address = stringField(obj, "address", server->address);
        if (obj.contains("port")) server->port = longField(obj, "port", server->port);
        // Settable back to empty on purpose: that is how a server is moved back to one shared
        // key space at the bucket root.
        if (obj.contains("homeDirectory")) server->homeDirectory = stringField(obj, "homeDirectory", server->homeDirectory);
        if (obj.contains("userIds")) server->userIds = stringArray(obj, "userIds");
        if (obj.contains("userGroups")) server->userGroups = stringArray(obj, "userGroups");
        if (obj.contains("hostKey")) server->hostKey = stringField(obj, "hostKey", server->hostKey);
        if (obj.contains("pasvMin")) server->pasvMin = longField(obj, "pasvMin", server->pasvMin);
        if (obj.contains("pasvMax")) server->pasvMax = longField(obj, "pasvMax", server->pasvMax);

        if (obj.contains("bucket")) {
            const auto bucketName = stringField(obj, "bucket");
            const auto bucket = Database::RepositoryFactory::instance().esmRepository()->findBucketByName(bucketName);
            if (!bucket.has_value()) return EtsServer::ErrorResponse(req, status::not_found, "Bucket not found: " + bucketName);
            server->bucketName = bucket->name;
            server->bucketErn = bucket->ern;
        }

        const auto stored = repo->upsertServer(*server);
        log_info << "ETS updated transfer server, serverId: " << stored.serverId;

        return EtsServer::JsonResponse(req, status::ok, boost::json::serialize(toJson(stored)));
    }

    static response<string_body> handleListServers(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-servers");

        AuthResult auth;
        if (const auto denied = requireAdmin(req, auth)) return *denied;

        boost::json::value jv;
        if (const auto err = EtsServer::ParseJsonBody(req, jv)) return *err;

        std::string prefix;
        if (jv.is_object()) prefix = stringField(jv.as_object(), "prefix");

        boost::json::array servers;
        for (const auto &server: Database::RepositoryFactory::instance().etsRepository()->listServers(prefix)) {
            servers.push_back(toJson(server));
        }

        return EtsServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::object{{"servers", servers}}));
    }

    static response<string_body> handleGetServer(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-server");

        AuthResult auth;
        if (const auto denied = requireAdmin(req, auth)) return *denied;

        boost::json::value jv;
        if (const auto err = EtsServer::ParseJsonBody(req, jv)) return *err;
        if (!jv.is_object()) return EtsServer::ErrorResponse(req, status::bad_request, "Expected a JSON object body");

        const auto serverId = stringField(jv.as_object(), "serverId");
        const auto server = Database::RepositoryFactory::instance().etsRepository()->findServerByServerId(serverId);
        if (!server.has_value()) {
            return EtsServer::ErrorResponse(req, status::not_found, "Transfer server not found: " + serverId);
        }

        return EtsServer::JsonResponse(req, status::ok, boost::json::serialize(toJson(*server)));
    }

    static response<string_body> handleDeleteServer(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-server");

        AuthResult auth;
        if (const auto denied = requireAdmin(req, auth)) return *denied;

        boost::json::value jv;
        if (const auto err = EtsServer::ParseJsonBody(req, jv)) return *err;
        if (!jv.is_object()) return EtsServer::ErrorResponse(req, status::bad_request, "Expected a JSON object body");

        const auto serverId = stringField(jv.as_object(), "serverId");
        const auto repo = Database::RepositoryFactory::instance().etsRepository();
        if (!repo->serverExists(serverId)) {
            return EtsServer::ErrorResponse(req, status::not_found, "Transfer server not found: " + serverId);
        }

        // Deleting the definition is what stops the process: the reconciler runs whatever is
        // defined and RUNNING, so a definition that no longer exists is torn down on the next tick.
        repo->deleteServer(serverId);
        log_info << "ETS deleted transfer server, serverId: " << serverId;

        return EtsServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::object{{"serverId", serverId}, {"deleted", true}}));
    }

    // start-server and stop-server only record intent; euclid-mgr's reconciler is what acts on it.
    static response<string_body> handleSetState(const request<string_body> &req, const TransferServerState desired) {

        // Labelled by the action the client asked for rather than by this function's name, so
        // start-server and stop-server are told apart the way every other action is.
        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method",
                                                  desired == TransferServerState::RUNNING ? "start-server" : "stop-server");

        AuthResult auth;
        if (const auto denied = requireAdmin(req, auth)) return *denied;

        boost::json::value jv;
        if (const auto err = EtsServer::ParseJsonBody(req, jv)) return *err;
        if (!jv.is_object()) return EtsServer::ErrorResponse(req, status::bad_request, "Expected a JSON object body");

        const auto serverId = stringField(jv.as_object(), "serverId");
        const auto repo = Database::RepositoryFactory::instance().etsRepository();
        auto server = repo->findServerByServerId(serverId);
        if (!server.has_value()) {
            return EtsServer::ErrorResponse(req, status::not_found, "Transfer server not found: " + serverId);
        }

        server->desiredState = desired;
        const auto stored = repo->upsertServer(*server);
        log_info << "ETS set transfer server state, serverId: " << serverId << ", desiredState: " << TransferServerStateToString(desired);

        return EtsServer::JsonResponse(req, status::ok, boost::json::serialize(toJson(stored)));
    }

    // ── Request dispatcher ───────────────────────────────────────────────────

    static response<string_body> dispatch(const request<string_body> &req) {

        const auto action = std::string(req["x-euclid-action"]);
        if (action.empty()) {
            return EtsServer::ErrorResponse(req, status::bad_request, "Missing x-euclid-action header");
        }
        log_debug << "ETS action=" << action;

        if (action == "create-server") return handleCreateServer(req);
        if (action == "update-server") return handleUpdateServer(req);
        if (action == "list-servers") return handleListServers(req);
        if (action == "get-server") return handleGetServer(req);
        if (action == "delete-server") return handleDeleteServer(req);
        if (action == "start-server") return handleSetState(req, TransferServerState::RUNNING);
        if (action == "stop-server") return handleSetState(req, TransferServerState::STOPPED);
        if (action == "get-metrics") return EtsServer::MetricsResponse(req);

        return EtsServer::ErrorResponse(req, status::not_found, "Action not implemented: " + action);
    }

    // ── EtsServer ────────────────────────────────────────────────────────────

    EtsServer::EtsServer(std::string socketPath, const int threads) : HttpActionServer("ETS", std::move(socketPath), threads) {}

    response<string_body> EtsServer::Dispatch(const request<string_body> &req) {
        return dispatch(req);
    }

}// namespace Euclid::ETS
