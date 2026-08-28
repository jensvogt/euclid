// Euclid includes
#include <EkmServer.h>

#include "euclid/dto/ekm/mapper/EkmMapper.h"

namespace Euclid::EKM {

    namespace beast = boost::beast;
    namespace http = beast::http;

    // ── Helpers ──────────────────────────────────────────────────────────────

    namespace {
        // Looks up the caller identity resolved by EkmServer::Authenticate(), by user ID.
        // Distinguishing an expired token lets handlers return a more specific error than a plain 401.
        struct AuthResult {
            std::optional<Database::Entity::EAM::User> user;
            bool tokenExpired{false};
            std::string denialReason;
        };

        // Timer/counter names shared by every handler below - one series per action, labeled
        // "method"=<action>, e.g. name="queues-service-time" labelName="method" labelValue="send-message".
        constexpr auto kServiceTimer = "ekm-service-time";
        constexpr auto kServiceCounter = "ekm-service-count";
    }// namespace

    static AuthResult authenticate(const request<string_body> &req) {
        const auto auth = EkmServer::Authenticate(req);
        if (!auth.subject.has_value()) {
            return {.user = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason};
        }
        return {.user = Database::RepositoryFactory::instance().eamRepository()->findUserByUserId(*auth.subject)};
    }

    static response<string_body> unauthorized(const request<string_body> &req, const AuthResult &auth) {
        return EkmServer::Unauthorized(req, {.subject = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason});
    }

    // Fills in the caller identity shared by every response DTO's "metadata" object. The
    // request ID that correlates this response with its request travels as the
    // "x-euclid-request-id" header instead (set centrally in HttpActionServer::JsonResponse).
    static void applyMetadata(Dto::BaseDto &response, const Database::Entity::EAM::User &user) {
        response.user = user.userId;
        response.accountId = user.accountId;
        response.region = user.region;
    }

    // ── Action handlers ──────────────────────────────────────────────────────
    // Each handler parses whatever fields it needs out of the JSON request body.
    // Return a fully formed HTTP response.

    static response<string_body> handleCreateKey(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "create-queue");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EkmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EKM::CreateKeyRequest>(jv);
        const auto ns = std::string(req["x-euclid-namespace"]);

        Database::Entity::EKM::Key key;
        key.accountId = auth.user->accountId;
        key.nameSpace = ns;
        key.algorithm = request.algorithm;
        key.length = request.length;

        const auto saved = Database::RepositoryFactory::instance().ekmRepository()->upsertKey(key);

        Dto::EKM::CreateKeyResponse response;
        response.name = saved.name;
        response.ern = saved.ern;
        return EkmServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleListKeys(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-queues");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EkmServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EKM::ListKeysRequest>(jv);
        log_info << "EKM ListKeys" << (!request.prefix.empty() ? ", prefix: " + request.prefix : "");

        const auto nameSpace = std::string(req["x-euclid-namespace"]);
        const auto repo = Database::RepositoryFactory::instance().ekmRepository();
        const std::vector<Database::Entity::EKM::Key> keys = repo->listKeys(auth.user->accountId, nameSpace, request.prefix, request.pageSize, request.pageIndex, request.sortColumn);
        log_info << "EKM key list, count: " << keys.size();

        Dto::EKM::ListKeysResponse response;
        response.keys = Dto::EKM::EkmMapper::toDto(keys);
        response.total = repo->countKeys(auth.user->accountId, nameSpace);

        return EkmServer::JsonResponse(req, status::ok, response.toJson());
    }

    // ── Request dispatcher ───────────────────────────────────────────────────

    namespace {
        // Commands the EQS service accepts via the "x-euclid-action" header.
        enum class Command {
            Unknown,
            CreateKey,
            ListKeys
        };
    }

    static Command commandFromString(const std::string &action) {
        if (action == "create-key") return Command::CreateKey;
        if (action == "list-keys") return Command::ListKeys;
        return Command::Unknown;
    }

    static response<string_body> dispatch(const request<string_body> &req) {

        const auto action = std::string(req["x-euclid-action"]);
        if (action.empty()) {
            return EkmServer::ErrorResponse(req, status::bad_request, "Missing x-euclid-action header");
        }
        log_debug << "EKM action=" << action;

        switch (commandFromString(action)) {

            case Command::CreateKey:
                return handleCreateKey(req);

            case Command::ListKeys:
                return handleListKeys(req);

            case Command::Unknown:
            default:
                log_warning << "Unknown action: " << action;
                return EkmServer::ErrorResponse(req, status::not_found, "Action not implemented: " + action);
        }
    }

    // ── EkmServer ────────────────────────────────────────────────────────────

    EkmServer::EkmServer(std::string socketPath, const int threads) : HttpActionServer("EKM", std::move(socketPath), threads) {}

    EkmServer::~EkmServer() = default;

    response<string_body> EkmServer::Dispatch(const request<string_body> &req) {
        return dispatch(req);
    }

}// namespace Euclid::EQS