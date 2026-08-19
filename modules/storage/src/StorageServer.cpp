// Euclid includes
#include <StorageServer.h>

namespace Euclid::Storage {

    namespace beast = boost::beast;
    namespace http = beast::http;

    // ── Helpers ──────────────────────────────────────────────────────────────

    namespace {
        // Looks up the caller identity resolved by SqsServer::Authenticate(), by user ID.
        // Distinguishing an expired token lets handlers return a more specific error than a plain 401.
        struct AuthResult {
            std::optional<Database::Entity::Access::User> user;
            bool tokenExpired{false};
            std::string denialReason;
        };

        // Timer/counter names shared by every handler below - one series per action, labeled
        // "method"=<action>, e.g. name="queues-service-time" labelName="method" labelValue="send-message".
        constexpr auto kServiceTimer = "storage-service-time";
        constexpr auto kServiceCounter = "storage-service-count";
    }// namespace

    static AuthResult authenticate(const request<string_body> &req) {
        const auto auth = StorageServer::Authenticate(req);
        if (!auth.subject.has_value()) {
            return {.user = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason};
        }
        return {.user = Database::RepositoryFactory::instance().accessRepository()->findUserByUserId(*auth.subject)};
    }

    static response<string_body> unauthorized(const request<string_body> &req, const AuthResult &auth) {
        return StorageServer::Unauthorized(req, {.subject = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason});
    }

    // Fills in the caller identity shared by every response DTO's "metadata" object. The
    // request ID that correlates this response with its request travels as the
    // "x-euclid-request-id" header instead (set centrally in HttpActionServer::JsonResponse).
    static void applyMetadata(Dto::BaseDto &response, const Database::Entity::Access::User &user) {
        response.user = user.userId;
        response.accountId = user.accountId;
        response.region = user.region;
    }

    // ── Action handlers ──────────────────────────────────────────────────────
    // Each handler parses whatever fields it needs out of the JSON request body.
    // Return a fully formed HTTP response.

    static response<string_body> handleCreateBucket(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "create-bucket");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = StorageServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::Storage::CreateBucketRequest>(jv);

        Database::Entity::Storage::Bucket bucket;
        bucket.name = request.name;
        bucket.ern = Core::createStorageBucketErn(auth.user->accountId, request.name);
        bucket.region = auth.user->region;
        bucket.owner = auth.user->userId;

        // const auto saved = Database::RepositoryFactory::instance().sqsRepository()->upsertQueue(queue);
        //
        //Dto::SQS::CreateQueueResponse response;
        // response.name = saved.name;
        // response.ern = saved.ern;

        return StorageServer::JsonResponse(req, status::ok);
    }

    // ── Request dispatcher ───────────────────────────────────────────────────

    namespace {
        // Commands the SQS service accepts via the "x-euclid-action" header.
        enum class Command {
            Unknown,
            CreateBucket,
            GetMetrics
        };
    }

    static Command commandFromString(const std::string &action) {
        if (action == "create-bucket") return Command::CreateBucket;
        if (action == "get-metrics") return Command::GetMetrics;
        return Command::Unknown;
    }

    static response<string_body> dispatch(const request<string_body> &req) {

        const auto action = std::string(req["x-euclid-action"]);
        if (action.empty()) {
            return StorageServer::ErrorResponse(req, status::bad_request, "Missing x-euclid-action header");
        }
        log_debug << "SQS action=" << action;

        switch (commandFromString(action)) {

            case Command::CreateBucket:
                return handleCreateBucket(req);

            case Command::Unknown:
            default:
                return StorageServer::ErrorResponse(req, status::not_found, "Action not implemented: " + action);
        }
    }

    // ── StorageServer ────────────────────────────────────────────────────────────

    StorageServer::StorageServer(std::string socketPath, const int threads) : HttpActionServer("storage", std::move(socketPath), threads) {
        auto &scheduler = Core::Scheduler::instance();
        scheduler.Start();
        _lifecyleTaskId = scheduler.SchedulePeriodic("queues-reset-expired-messages", [] {
                                                         Database::RepositoryFactory::instance().sqsRepository()->resetExpiredMessages();
                                                     },
                                                     std::chrono::seconds(30));
    }

    StorageServer::~StorageServer() {
        Core::Scheduler::instance().Cancel(_lifecyleTaskId);
    }

    response<string_body> StorageServer::Dispatch(const request<string_body> &req) {
        return dispatch(req);
    }

}// namespace Euclid::SQS