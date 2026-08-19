// Euclid includes
#include <euclid/core/monitoring/MonitoringTimer.h>
#include <SqsServer.h>

namespace Euclid::SQS {

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
        // "method"=<action>, e.g. name="sqs-service-time" labelName="method" labelValue="send-message".
        constexpr auto kServiceTimer = "sqs-service-time";
        constexpr auto kServiceCounter = "sqs-service-count";
    }// namespace

    static AuthResult authenticate(const request<string_body> &req) {
        const auto auth = SqsServer::Authenticate(req);
        if (!auth.subject.has_value()) {
            return {.user = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason};
        }
        return {.user = Database::RepositoryFactory::instance().accessRepository()->findUserByUserId(*auth.subject)};
    }

    static response<string_body> unauthorized(const request<string_body> &req, const AuthResult &auth) {
        return SqsServer::Unauthorized(req, {.subject = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason});
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

    static response<string_body> handleCreateQueue(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "create-queue");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = SqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::SQS::CreateQueueRequest>(jv);

        Database::Entity::SQS::Queue dlqSaved;
        if (!request.dlqName.empty()) {
            Database::Entity::SQS::Queue dlQueue;
            dlQueue.name = request.dlqName;
            dlQueue.ern = Core::createSqsQueueErn(auth.user->accountId, request.dlqName);
            dlQueue.visibility = request.visibility;
            dlQueue.maxMessageLength = 1024 * 1024;
            dlQueue.maxReceiveCount = request.maxRetries;
            dlQueue.region = auth.user->region;
            dlQueue.owner = auth.user->userId;

            dlqSaved = Database::RepositoryFactory::instance().sqsRepository()->upsertQueue(dlQueue);
        }

        Database::Entity::SQS::Queue queue;
        queue.name = request.name;
        queue.ern = Core::createSqsQueueErn(auth.user->accountId, request.name);
        queue.visibility = request.visibility;
        queue.maxMessageLength = 1024 * 1024;
        queue.maxReceiveCount = request.maxRetries;
        queue.region = auth.user->region;
        queue.owner = auth.user->userId;
        queue.deadLetterQueueErn = dlqSaved.ern;

        const auto saved = Database::RepositoryFactory::instance().sqsRepository()->upsertQueue(queue);

        Dto::SQS::CreateQueueResponse response;
        response.name = saved.name;
        response.ern = saved.ern;
        return SqsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleDeleteQueue(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-queue");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = SqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = Dto::SQS::DeleteQueueRequest::fromJson(req.body());
        log_info << "SQS DeleteQueue, ern: " << request.ern;

        Database::RepositoryFactory::instance().sqsRepository()->deleteQueueByErn(request.ern);

        return SqsServer::JsonResponse(req, status::ok);
    }

    static response<string_body> handleGetQueueErn(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-queue-ern");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = SqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::SQS::GetQueueErnRequest>(jv);
        log_info << "SQS GetQueueErn, name: " << request.name;

        const std::optional<Database::Entity::SQS::Queue> queue = Database::RepositoryFactory::instance().sqsRepository()->findQueueByName(request.name);
        log_debug << "Got SQS queue, name: " << request.name << ", ern: " << (queue.has_value() ? queue->ern : "(none)");

        if (!queue.has_value()) {
            return SqsServer::ErrorResponse(req, status::not_found, "Queue not found, name: " + request.name);
        }

        Dto::SQS::GetQueueErnResponse response;
        response.ern = queue->ern;

        return SqsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleListQueues(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-queues");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = SqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::SQS::ListQueueRequest>(jv);
        log_info << "SQS ListQueues" << (!request.prefix.empty() ? ", prefix: " + request.prefix : "");

        const auto repo = Database::RepositoryFactory::instance().sqsRepository();
        const std::vector<Database::Entity::SQS::Queue> queues = repo->listQueues(request.prefix, request.pageSize, request.pageIndex, request.sortColumn);
        log_info << "Got queue list, count: " << queues.size();

        Dto::SQS::ListQueueResponse response;
        response.queues = Dto::SQS::SqsMapper::toDto(queues);
        response.total = repo->countQueues();

        return SqsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleSendMessage(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "send-message");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = SqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::SQS::SendMessageRequest>(jv);
        log_info << "SQS SendMessage queueErn: " << request.ern;

        const std::string messageId = Core::UuidUtils::CreateRandomUuid();
        std::map<std::string, Database::Entity::SQS::Variant> attributes;
        for (const auto &[key, variant]: request.attributes) {
            attributes[key] = Dto::SQS::SqsMapper::toEntity(variant);
        }
        const auto repo = Database::RepositoryFactory::instance().sqsRepository();
        const Database::Entity::SQS::Message message = repo->sendMessage(messageId, request.ern, request.body, attributes);

        Dto::SQS::SendMessageResponse response;
        response.messageId = message.messageId;
        response.md5Body = message.md5Body;
        response.md5Attributes = message.md5Attributes;

        return SqsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleReceiveMessage(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "receive-messages");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = SqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::SQS::ReceiveMessagesRequest>(jv);
        log_info << "SQS ReceiveMessages ern: " << request.ern;

        const auto repo = Database::RepositoryFactory::instance().sqsRepository();
        std::vector<Database::Entity::SQS::Message> messages = repo->receiveMessages(request.ern, request.maxCount, request.waitTime);

        Dto::SQS::ReceiveMessagesResponse response;
        response.messages = Dto::SQS::SqsMapper::toDto(messages);
        response.total = repo->countMessages(request.ern);

        log_info << "SQS ReceiveMessages ern: " << request.ern << ", count: " << response.messages.size() << ", total: " << response.total;

        return SqsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleDeleteMessage(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-message");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = SqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::SQS::DeleteMessageRequest>(jv);
        log_info << "SQS DeleteMessage receiptHandle: " << request.receiptHandle;

        const auto repo = Database::RepositoryFactory::instance().sqsRepository();
        repo->deleteMessage(request.receiptHandle);

        return SqsServer::JsonResponse(req, status::ok);
    }

    static response<string_body> handlePurgeQueue(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "purge-queue");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = SqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::SQS::PurgeQueueRequest>(jv);
        log_info << "SQS PurgeQueue ern: " << request.ern;

        const auto repo = Database::RepositoryFactory::instance().sqsRepository();
        repo->purgeQueue(request.ern);

        return SqsServer::JsonResponse(req, status::ok);
    }

    static response<string_body> handlePurgeAllQueues(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "purge-all-queues");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = SqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::SQS::PurgeAllQueuesRequest>(jv);
        log_info << "SQS PurgeAllQueues";

        const auto repo = Database::RepositoryFactory::instance().sqsRepository();
        repo->purgeAllQueues(request.region, request.accountId);

        return SqsServer::JsonResponse(req, status::ok);
    }

    static response<string_body> handleGetMessageCount(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-message-count");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = SqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::SQS::GetMessageCountRequest>(jv);
        log_info << "SQS GetMessageCount, ern: " << request.ern;

        const auto repo = Database::RepositoryFactory::instance().sqsRepository();
        const std::optional<Database::Entity::SQS::Queue> queue = repo->findQueueByErn(request.ern);
        if (!queue.has_value()) {
            return SqsServer::ErrorResponse(req, status::not_found, "Queue not found, ern: " + request.ern);
        }

        Dto::SQS::GetMessageCountResponse response;
        response.ern = request.ern;
        response.available = queue->available;
        response.delayed = queue->delayed;
        response.invisible = queue->invisible;
        return SqsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleGetMessageAttribute(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-message-attribute");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = SqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::SQS::GetMessageAttributeRequest>(jv);
        log_info << "SQS GetMessageAttribute, messageId: " << request.messageId << ", name: " << request.name;

        const auto repo = Database::RepositoryFactory::instance().sqsRepository();
        const std::optional<Database::Entity::SQS::Message> message = repo->findMessageByName(request.messageId);
        if (!message.has_value()) {
            return SqsServer::ErrorResponse(req, status::not_found, "Message not found, messageId: " + request.messageId);
        }

        const auto attribute = message->attributes.find(request.name);
        if (attribute == message->attributes.end()) {
            return SqsServer::ErrorResponse(req, status::not_found, "Attribute not found, name: " + request.name);
        }

        Dto::SQS::GetMessageAttributeResponse response;
        response.messageId = request.messageId;
        response.name = request.name;
        response.value = Dto::SQS::SqsMapper::toDto(attribute->second);

        return SqsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleGetQueueAttributes(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-metadata");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = SqsServer::ParseJsonBody(req, jv)) return *err;

        log_info << "SQS GetQueueAttributes url=" << Core::GetStringValue(jv, "QueueUrl");

        const boost::json::object body{
                {"Attributes", boost::json::object{}},
                {"ResponseMetadata", boost::json::object{{"RequestId", SqsServer::RequestId()}}}
        };
        return SqsServer::JsonResponse(req, status::ok, boost::json::serialize(body));
    }

    static response<string_body> handleSetQueueAttributes(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "add-metadata");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = SqsServer::ParseJsonBody(req, jv)) return *err;

        log_info << "SQS SetQueueAttributes url=" << Core::GetStringValue(jv, "QueueUrl");

        const boost::json::object body{
                {"ResponseMetadata", boost::json::object{{"RequestId", SqsServer::RequestId()}}}
        };
        return SqsServer::JsonResponse(req, status::ok, boost::json::serialize(body));
    }

    // ── Request dispatcher ───────────────────────────────────────────────────

    namespace {
        // Commands the SQS service accepts via the "x-euclid-action" header.
        enum class Command {
            Unknown,
            CreateQueue,
            DeleteQueue,
            GetQueueErn,
            GetMessageCount,
            GetMessageAttribute,
            ListQueues,
            SendMessage,
            ReceiveMessages,
            DeleteMessage,
            PurgeQueue,
            PurgeAllQueues,
            GetMetadata,
            AddMetadata,
            GetMetrics
        };
    }

    static Command commandFromString(const std::string &action) {
        if (action == "create-queue") return Command::CreateQueue;
        if (action == "delete-queue") return Command::DeleteQueue;
        if (action == "get-queue-ern") return Command::GetQueueErn;
        if (action == "list-queues") return Command::ListQueues;
        if (action == "send-message") return Command::SendMessage;
        if (action == "receive-messages") return Command::ReceiveMessages;
        if (action == "delete-message") return Command::DeleteMessage;
        if (action == "purge-queue") return Command::PurgeQueue;
        if (action == "purge-all-queues") return Command::PurgeAllQueues;
        if (action == "get-message-count") return Command::GetMessageCount;
        if (action == "get-message-attribute") return Command::GetMessageAttribute;
        if (action == "get-metadata") return Command::GetMetadata;
        if (action == "add-metadata") return Command::AddMetadata;
        if (action == "get-metrics") return Command::GetMetrics;
        return Command::Unknown;
    }

    static response<string_body> dispatch(const request<string_body> &req) {

        const auto action = std::string(req["x-euclid-action"]);
        if (action.empty()) {
            return SqsServer::ErrorResponse(req, status::bad_request, "Missing x-euclid-action header");
        }
        log_debug << "SQS action=" << action;

        switch (commandFromString(action)) {

            case Command::CreateQueue:
                return handleCreateQueue(req);

            case Command::DeleteQueue:
                return handleDeleteQueue(req);

            case Command::GetQueueErn:
                return handleGetQueueErn(req);

            case Command::ListQueues:
                return handleListQueues(req);

            case Command::SendMessage:
                return handleSendMessage(req);

            case Command::ReceiveMessages:
                return handleReceiveMessage(req);

            case Command::DeleteMessage:
                return handleDeleteMessage(req);

            case Command::PurgeQueue:
                return handlePurgeQueue(req);

            case Command::PurgeAllQueues:
                return handlePurgeAllQueues(req);

            case Command::GetMetadata:
                return handleGetQueueAttributes(req);

            case Command::AddMetadata:
                return handleSetQueueAttributes(req);

            case Command::GetMessageCount:
                return handleGetMessageCount(req);

            case Command::GetMessageAttribute:
                return handleGetMessageAttribute(req);

            case Command::GetMetrics:
                return SqsServer::MetricsResponse(req);

            case Command::Unknown:
            default:
                return SqsServer::ErrorResponse(req, status::not_found, "Action not implemented: " + action);
        }
    }

    // ── SqsServer ────────────────────────────────────────────────────────────

    SqsServer::SqsServer(std::string socketPath, const int threads) : HttpActionServer("SQS", std::move(socketPath), threads) {
        auto &scheduler = Core::Scheduler::instance();
        scheduler.Start();
        _resetMessagesTaskId = scheduler.SchedulePeriodic("sqs-reset-expired-messages", [] {
                                                              Database::RepositoryFactory::instance().sqsRepository()->resetExpiredMessages();
                                                          },
                                                          std::chrono::seconds(30));
    }

    SqsServer::~SqsServer() {
        Core::Scheduler::instance().Cancel(_resetMessagesTaskId);
    }

    response<string_body> SqsServer::Dispatch(const request<string_body> &req) {
        return dispatch(req);
    }

}// namespace Euclid::SQS