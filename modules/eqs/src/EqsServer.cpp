// Euclid includes
#include <EqsServer.h>

namespace Euclid::EQS {

    namespace beast = boost::beast;
    namespace http = beast::http;

    // ── Helpers ──────────────────────────────────────────────────────────────

    namespace {
        // Looks up the caller identity resolved by EqsServer::Authenticate(), by user ID.
        // Distinguishing an expired token lets handlers return a more specific error than a plain 401.
        struct AuthResult {
            std::optional<Database::Entity::EAM::User> user;
            bool tokenExpired{false};
            std::string denialReason;
        };

        // Timer/counter names shared by every handler below - one series per action, labeled
        // "method"=<action>, e.g. name="queues-service-time" labelName="method" labelValue="send-message".
        constexpr auto kServiceTimer = "queues-service-time";
        constexpr auto kServiceCounter = "queues-service-count";
    }// namespace

    static AuthResult authenticate(const request<string_body> &req) {
        const auto auth = EqsServer::Authenticate(req);
        if (!auth.subject.has_value()) {
            return {.user = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason};
        }
        return {.user = Database::RepositoryFactory::instance().eamRepository()->findUserByUserId(*auth.subject)};
    }

    static response<string_body> unauthorized(const request<string_body> &req, const AuthResult &auth) {
        return EqsServer::Unauthorized(req, {.subject = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason});
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

    static response<string_body> handleCreateQueue(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "create-queue");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EQS::CreateQueueRequest>(jv);

        Database::Entity::EQS::Queue dlqSaved;
        if (!request.dlqName.empty()) {
            Database::Entity::EQS::Queue dlQueue;
            dlQueue.name = request.dlqName;
            dlQueue.ern = Core::createSqsQueueErn(auth.user->accountId, request.dlqName);
            dlQueue.visibility = request.visibility;
            dlQueue.maxMessageLength = request.maxMessageLength;
            dlQueue.maxReceiveCount = request.maxRetries;
            dlQueue.region = auth.user->region;
            dlQueue.owner = auth.user->userId;
            dlQueue.delay = request.delay;

            dlqSaved = Database::RepositoryFactory::instance().eqsRepository()->upsertQueue(dlQueue);
        }

        Database::Entity::EQS::Queue queue;
        queue.name = request.name;
        queue.ern = Core::createSqsQueueErn(auth.user->accountId, request.name);
        queue.visibility = request.visibility;
        queue.maxMessageLength = request.maxMessageLength;
        queue.maxReceiveCount = request.maxRetries;
        queue.region = auth.user->region;
        queue.owner = auth.user->userId;
        queue.deadLetterQueueErn = dlqSaved.ern;
        queue.delay = request.delay;

        const auto saved = Database::RepositoryFactory::instance().eqsRepository()->upsertQueue(queue);

        Dto::EQS::CreateQueueResponse response;
        response.name = saved.name;
        response.ern = saved.ern;
        return EqsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleDeleteQueue(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-queue");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = Dto::EQS::DeleteQueueRequest::fromJson(req.body());
        log_info << "SQS DeleteQueue, ern: " << request.ern;

        Database::RepositoryFactory::instance().eqsRepository()->deleteQueueByErn(request.ern);

        return EqsServer::JsonResponse(req, status::ok);
    }

    static response<string_body> handleGetQueueErn(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-queue-ern");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EQS::GetQueueErnRequest>(jv);
        log_info << "SQS GetQueueErn, name: " << request.name;

        const std::optional<Database::Entity::EQS::Queue> queue = Database::RepositoryFactory::instance().eqsRepository()->findQueueByName(request.name);
        log_debug << "Got SQS queue, name: " << request.name << ", ern: " << (queue.has_value() ? queue->ern : "(none)");

        if (!queue.has_value()) {
            return EqsServer::ErrorResponse(req, status::not_found, "Queue not found, name: " + request.name);
        }

        Dto::EQS::GetQueueErnResponse response;
        response.ern = queue->ern;

        return EqsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleListQueues(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-queues");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EQS::ListQueueRequest>(jv);
        log_info << "SQS ListQueues" << (!request.prefix.empty() ? ", prefix: " + request.prefix : "");

        const auto repo = Database::RepositoryFactory::instance().eqsRepository();
        const std::vector<Database::Entity::EQS::Queue> queues = repo->listQueues(request.prefix, request.pageSize, request.pageIndex, request.sortColumn);
        log_info << "Got queue list, count: " << queues.size();

        Dto::EQS::ListQueueResponse response;
        response.queues = Dto::EQS::EqsMapper::toDto(queues);
        response.total = repo->countQueues();

        return EqsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleListMessages(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-messages");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EQS::ListMessagesRequest>(jv);
        log_info << "SQS ListMessages, queueErn: " << request.queueErn;

        const auto repo = Database::RepositoryFactory::instance().eqsRepository();
        const std::vector<Database::Entity::EQS::Message> messages = repo->listMessages(request.queueErn, request.pageSize, request.pageIndex, request.sortColumn);
        log_info << "Got message list, count: " << messages.size();

        Dto::EQS::ListMessagesResponse response;
        response.messages = Dto::EQS::EqsMapper::toDto(messages);
        response.total = repo->countMessages(request.queueErn);

        return EqsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleSendMessage(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "send-message");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EQS::SendMessageRequest>(jv);
        log_info << "SQS SendMessage queueErn: " << request.queueErn;

        const std::string messageId = Core::UuidUtils::CreateRandomUuid();
        const std::string ern = Core::createSqsMessageErn(auth.user.value().accountId, messageId);
        std::map<std::string, Database::Entity::EQS::Variant> attributes;
        for (const auto &[key, variant]: request.attributes) {
            attributes[key] = Dto::EQS::EqsMapper::toEntity(variant);
        }
        const auto repo = Database::RepositoryFactory::instance().eqsRepository();
        const Database::Entity::EQS::Message message = repo->sendMessage(messageId, ern, request.queueErn, request.body, attributes, Database::Entity::EQS::MessagePriorityFromString(request.priority));

        Dto::EQS::SendMessageResponse response;
        response.messageId = message.messageId;
        response.md5Body = message.md5Body;
        response.md5Attributes = message.md5Attributes;

        return EqsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleReceiveMessage(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "receive-messages");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EQS::ReceiveMessagesRequest>(jv);
        log_info << "SQS ReceiveMessages ern: " << request.ern;

        const auto repo = Database::RepositoryFactory::instance().eqsRepository();
        std::vector<Database::Entity::EQS::Message> messages = repo->receiveMessages(request.ern, request.maxCount, request.waitTime);

        Dto::EQS::ReceiveMessagesResponse response;
        response.messages = Dto::EQS::EqsMapper::toDto(messages);
        response.total = repo->countMessages(request.ern);

        log_info << "SQS ReceiveMessages ern: " << request.ern << ", count: " << response.messages.size() << ", total: " << response.total;

        return EqsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleDeleteMessage(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-message");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EQS::DeleteMessageRequest>(jv);
        log_info << "SQS DeleteMessage receiptHandle: " << request.receiptHandle;

        const auto repo = Database::RepositoryFactory::instance().eqsRepository();
        repo->deleteMessage(request.receiptHandle);

        return EqsServer::JsonResponse(req, status::ok);
    }

    static response<string_body> handlePurgeQueue(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "purge-queue");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EQS::PurgeQueueRequest>(jv);
        log_info << "SQS PurgeQueue ern: " << request.ern;

        const auto repo = Database::RepositoryFactory::instance().eqsRepository();
        repo->purgeQueue(request.ern);

        return EqsServer::JsonResponse(req, status::ok);
    }

    static response<string_body> handlePurgeAllQueues(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "purge-all-queues");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EQS::PurgeAllQueuesRequest>(jv);
        log_info << "SQS PurgeAllQueues";

        const auto repo = Database::RepositoryFactory::instance().eqsRepository();
        repo->purgeAllQueues(request.region, request.accountId);

        return EqsServer::JsonResponse(req, status::ok);
    }

    static response<string_body> handleGetMessageCount(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-message-count");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EQS::GetMessageCountRequest>(jv);
        log_info << "SQS GetMessageCount, ern: " << request.ern;

        const auto repo = Database::RepositoryFactory::instance().eqsRepository();
        const std::optional<Database::Entity::EQS::Queue> queue = repo->findQueueByErn(request.ern);
        if (!queue.has_value()) {
            return EqsServer::ErrorResponse(req, status::not_found, "Queue not found, ern: " + request.ern);
        }

        Dto::EQS::GetMessageCountResponse response;
        response.ern = request.ern;
        response.available = queue->available;
        response.delayed = queue->delayed;
        response.invisible = queue->invisible;
        response.total = queue->available + queue->delayed + queue->invisible;
        return EqsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleGetQueueMetadata(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-queue-metadata");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EQS::GetQueueMetadataRequest>(jv);
        log_info << "SQS GetQueueMetadata, ern: " << request.ern;

        const auto repo = Database::RepositoryFactory::instance().eqsRepository();
        const std::optional<Database::Entity::EQS::Queue> queue = repo->findQueueByErn(request.ern);
        if (!queue.has_value()) {
            return EqsServer::ErrorResponse(req, status::not_found, "Queue not found, ern: " + request.ern);
        }

        Dto::EQS::GetQueueMetadataResponse response;
        response.region = queue->region;
        response.accountId = Core::accountIdFromErn(queue->ern);
        response.owner = queue->owner;
        // Queues aren't namespace-scoped yet (no such field on Database::Entity::SQS::Queue), so
        // this is always empty for now rather than reporting something fabricated.
        response.nameSpace = "";
        response.name = queue->name;
        response.ern = queue->ern;
        response.size = queue->size;
        // "Total" across every message state, unlike get-message-count's breakdown into the three
        // individual available/delayed/invisible counts.
        response.messages = queue->available + queue->delayed + queue->invisible;
        return EqsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleGetMessageAttribute(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-message-attribute");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EQS::GetMessageAttributeRequest>(jv);
        log_info << "SQS GetMessageAttribute, messageId: " << request.messageId << ", name: " << request.name;

        const auto repo = Database::RepositoryFactory::instance().eqsRepository();
        const std::optional<Database::Entity::EQS::Message> message = repo->findMessageByName(request.messageId);
        if (!message.has_value()) {
            return EqsServer::ErrorResponse(req, status::not_found, "Message not found, messageId: " + request.messageId);
        }

        const auto attribute = message->attributes.find(request.name);
        if (attribute == message->attributes.end()) {
            return EqsServer::ErrorResponse(req, status::not_found, "Attribute not found, name: " + request.name);
        }

        Dto::EQS::GetMessageAttributeResponse response;
        response.messageId = request.messageId;
        response.name = request.name;
        response.value = Dto::EQS::EqsMapper::toDto(attribute->second);

        return EqsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleGetMessageMetadata(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-message-metadata");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EQS::GetMessageMetadataRequest>(jv);
        log_info << "SQS GetMessageMetadata, messageId: " << request.messageId;

        const auto repo = Database::RepositoryFactory::instance().eqsRepository();
        const std::optional<Database::Entity::EQS::Message> message = repo->findMessageByName(request.messageId);
        if (!message.has_value()) {
            return EqsServer::ErrorResponse(req, status::not_found, "Message not found, messageId: " + request.messageId);
        }

        Dto::EQS::GetMessageMetadataResponse response;
        response.messageId = message->messageId;
        response.queueErn = message->queueErn;
        response.receiptHandle = message->receiptHandle;
        response.status = Database::Entity::EQS::MessageStatusToString(message->status);
        response.priority = Database::Entity::EQS::MessagePriorityToString(message->priority);
        response.size = message->size;
        response.receivedCount = message->receivedCount;
        response.visibilityTimeout = message->visibilityTimeout;
        response.contentType = message->contentType;
        response.md5Body = message->md5Body;
        response.md5Attributes = message->md5Attributes;
        response.created = Core::DateTimeUtils::ToISO8601(message->created);
        response.modified = Core::DateTimeUtils::ToISO8601(message->modified);
        return EqsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleGetQueueAttributes(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-metadata");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        log_info << "SQS GetQueueAttributes url=" << Core::GetStringValue(jv, "QueueUrl");

        const boost::json::object body{
                {"Attributes", boost::json::object{}},
                {"ResponseMetadata", boost::json::object{{"RequestId", EqsServer::RequestId()}}}
        };
        return EqsServer::JsonResponse(req, status::ok, boost::json::serialize(body));
    }

    static response<string_body> handleSetQueueAttributes(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "add-metadata");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        log_info << "SQS SetQueueAttributes url=" << Core::GetStringValue(jv, "QueueUrl");

        const boost::json::object body{
                {"ResponseMetadata", boost::json::object{{"RequestId", EqsServer::RequestId()}}}
        };
        return EqsServer::JsonResponse(req, status::ok, boost::json::serialize(body));
    }

    static response<string_body> handleAddQueueTag(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "add-queue-tag");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto [ern, key, value] = boost::json::value_to<Dto::EQS::AddQueueTagRequest>(jv);
        log_info << "EQS AddQueueTag, ern: " << ern << ", key: " << key;

        const auto repo = Database::RepositoryFactory::instance().eqsRepository();
        std::optional<Database::Entity::EQS::Queue> queue = repo->findQueueByErn(ern);
        if (!queue.has_value()) {
            return EqsServer::ErrorResponse(req, status::not_found, "Queue not found, ern: " + ern);
        }
        queue->tags[key] = value;
        queue = repo->upsertQueue(queue.value());

        return EqsServer::JsonResponse(req, status::ok);
    }

    static response<string_body> handleDeleteQueueTag(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-queue-tag");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto [ern, key, value] = boost::json::value_to<Dto::EQS::AddQueueTagRequest>(jv);
        log_info << "EQS DeleteQueueTag, ern: " << ern << ", key: " << key;

        const auto repo = Database::RepositoryFactory::instance().eqsRepository();
        std::optional<Database::Entity::EQS::Queue> queue = repo->findQueueByErn(ern);
        if (!queue.has_value()) {
            return EqsServer::ErrorResponse(req, status::not_found, "Queue not found, ern: " + ern);
        }
        queue->tags.erase(key);
        queue = repo->upsertQueue(queue.value());

        return EqsServer::JsonResponse(req, status::ok);
    }

    // ── Request dispatcher ───────────────────────────────────────────────────

    namespace {
        // Commands the EQS service accepts via the "x-euclid-action" header.
        enum class Command {
            Unknown,
            CreateQueue,
            DeleteQueue,
            GetQueueErn,
            GetMessageCount,
            GetQueueMetadata,
            GetMessageAttribute,
            GetMessageMetadata,
            ListQueues,
            ListMessages,
            SendMessage,
            ReceiveMessages,
            DeleteMessage,
            PurgeQueue,
            PurgeAllQueues,
            GetMetadata,
            AddMetadata,
            AddQueueTag,
            DeleteQueueTag,
            GetMetrics
        };
    }

    static Command commandFromString(const std::string &action) {
        if (action == "create-queue") return Command::CreateQueue;
        if (action == "delete-queue") return Command::DeleteQueue;
        if (action == "get-queue-ern") return Command::GetQueueErn;
        if (action == "list-queues") return Command::ListQueues;
        if (action == "list-messages") return Command::ListMessages;
        if (action == "send-message") return Command::SendMessage;
        if (action == "receive-messages") return Command::ReceiveMessages;
        if (action == "delete-message") return Command::DeleteMessage;
        if (action == "purge-queue") return Command::PurgeQueue;
        if (action == "purge-all-queues") return Command::PurgeAllQueues;
        if (action == "get-message-count") return Command::GetMessageCount;
        if (action == "get-queue-metadata") return Command::GetQueueMetadata;
        if (action == "get-message-attribute") return Command::GetMessageAttribute;
        if (action == "get-message-metadata") return Command::GetMessageMetadata;
        if (action == "get-metadata") return Command::GetMetadata;
        if (action == "add-metadata") return Command::AddMetadata;
        if (action == "add-queue-tag") return Command::AddQueueTag;
        if (action == "delete-queue-tag") return Command::DeleteQueueTag;
        if (action == "get-metrics") return Command::GetMetrics;
        return Command::Unknown;
    }

    static response<string_body> dispatch(const request<string_body> &req) {

        const auto action = std::string(req["x-euclid-action"]);
        if (action.empty()) {
            return EqsServer::ErrorResponse(req, status::bad_request, "Missing x-euclid-action header");
        }
        log_debug << "EQS action=" << action;

        switch (commandFromString(action)) {

            case Command::CreateQueue:
                return handleCreateQueue(req);

            case Command::DeleteQueue:
                return handleDeleteQueue(req);

            case Command::GetQueueErn:
                return handleGetQueueErn(req);

            case Command::ListQueues:
                return handleListQueues(req);

            case Command::ListMessages:
                return handleListMessages(req);

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

            case Command::GetQueueMetadata:
                return handleGetQueueMetadata(req);

            case Command::GetMessageAttribute:
                return handleGetMessageAttribute(req);

            case Command::GetMessageMetadata:
                return handleGetMessageMetadata(req);

            case Command::AddQueueTag:
                return handleAddQueueTag(req);

            case Command::DeleteQueueTag:
                return handleDeleteQueueTag(req);

                case Command::GetMetrics:
                return EqsServer::MetricsResponse(req);

            case Command::Unknown:
            default:
                log_warning << "Unknown action: " << action;
                return EqsServer::ErrorResponse(req, status::not_found, "Action not implemented: " + action);
        }
    }

    // ── EqsServer ────────────────────────────────────────────────────────────

    EqsServer::EqsServer(std::string socketPath, const int threads) : HttpActionServer("EQS", std::move(socketPath), threads) {
        auto &scheduler = Core::Scheduler::instance();
        scheduler.Start();
        _resetMessagesTaskId = scheduler.SchedulePeriodic("queues-reset-expired-messages", [] {
                                                              Database::RepositoryFactory::instance().eqsRepository()->resetExpiredMessages();
                                                          },
                                                          std::chrono::seconds(30));
    }

    EqsServer::~EqsServer() {
        Core::Scheduler::instance().Cancel(_resetMessagesTaskId);
    }

    response<string_body> EqsServer::Dispatch(const request<string_body> &req) {
        return dispatch(req);
    }

}// namespace Euclid::EQS