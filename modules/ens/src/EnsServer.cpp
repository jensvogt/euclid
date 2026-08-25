// Euclid includes
#include <EnsServer.h>

namespace Euclid::ENS {

    namespace beast = boost::beast;
    namespace http = beast::http;

    // ── Helpers ──────────────────────────────────────────────────────────────

    namespace {
        // Looks up the caller identity resolved by EnsServer::Authenticate(), by user ID.
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
        const auto auth = EnsServer::Authenticate(req);
        if (!auth.subject.has_value()) {
            return {.user = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason};
        }
        return {.user = Database::RepositoryFactory::instance().eamRepository()->findUserByUserId(*auth.subject)};
    }

    static response<string_body> unauthorized(const request<string_body> &req, const AuthResult &auth) {
        return EnsServer::Unauthorized(req, {.subject = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason});
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

    // static response<string_body> handleCreateQueue(const request<string_body> &req) {
    //
    //     Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "create-queue");
    //
    //     const auto auth = authenticate(req);
    //     if (!auth.user.has_value()) return unauthorized(req, auth);
    //
    //     boost::json::value jv;
    //     if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;
    //
    //     const auto request = boost::json::value_to<Dto::ENS::CreateQueueRequest>(jv);
    //
    //     Database::Entity::ENS::Queue dlqSaved;
    //     if (!request.dlqName.empty()) {
    //         Database::Entity::ENS::Queue dlQueue;
    //         dlQueue.name = request.dlqName;
    //         dlQueue.ern = Core::createEnsQueueErn(auth.user->accountId, request.dlqName);
    //         dlQueue.visibility = request.visibility;
    //         dlQueue.maxMessageLength = request.maxMessageLength;
    //         dlQueue.maxReceiveCount = request.maxRetries;
    //         dlQueue.region = auth.user->region;
    //         dlQueue.owner = auth.user->userId;
    //         dlQueue.delay = request.delay;
    //
    //         dlqSaved = Database::RepositoryFactory::instance().ensRepository()->upsertQueue(dlQueue);
    //     }
    //
    //     Database::Entity::ENS::Queue queue;
    //     queue.name = request.name;
    //     queue.ern = Core::createEnsQueueErn(auth.user->accountId, request.name);
    //     queue.visibility = request.visibility;
    //     queue.maxMessageLength = request.maxMessageLength;
    //     queue.maxReceiveCount = request.maxRetries;
    //     queue.region = auth.user->region;
    //     queue.owner = auth.user->userId;
    //     queue.deadLetterQueueErn = dlqSaved.ern;
    //     queue.delay = request.delay;
    //
    //     const auto saved = Database::RepositoryFactory::instance().ensRepository()->upsertQueue(queue);
    //
    //     Dto::ENS::CreateQueueResponse response;
    //     response.name = saved.name;
    //     response.ern = saved.ern;
    //     return EnsServer::JsonResponse(req, status::ok, response.toJson());
    // }
    //
    // static response<string_body> handleDeleteQueue(const request<string_body> &req) {
    //
    //     Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-queue");
    //
    //     if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);
    //
    //     boost::json::value jv;
    //     if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;
    //
    //     const auto request = Dto::ENS::DeleteQueueRequest::fromJson(req.body());
    //     log_info << "SQS DeleteQueue, ern: " << request.ern;
    //
    //     Database::RepositoryFactory::instance().ensRepository()->deleteQueueByErn(request.ern);
    //
    //     return EnsServer::JsonResponse(req, status::ok);
    // }
    //
    // static response<string_body> handleGetQueueErn(const request<string_body> &req) {
    //
    //     Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-queue-ern");
    //
    //     if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);
    //
    //     boost::json::value jv;
    //     if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;
    //
    //     const auto request = boost::json::value_to<Dto::ENS::GetQueueErnRequest>(jv);
    //     log_info << "SQS GetQueueErn, name: " << request.name;
    //
    //     const std::optional<Database::Entity::ENS::Queue> queue = Database::RepositoryFactory::instance().ensRepository()->findQueueByName(request.name);
    //     log_debug << "Got SQS queue, name: " << request.name << ", ern: " << (queue.has_value() ? queue->ern : "(none)");
    //
    //     if (!queue.has_value()) {
    //         return EnsServer::ErrorResponse(req, status::not_found, "Queue not found, name: " + request.name);
    //     }
    //
    //     Dto::ENS::GetQueueErnResponse response;
    //     response.ern = queue->ern;
    //
    //     return EnsServer::JsonResponse(req, status::ok, response.toJson());
    // }
    //
    // static response<string_body> handleListQueues(const request<string_body> &req) {
    //
    //     Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-queues");
    //
    //     if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);
    //
    //     boost::json::value jv;
    //     if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;
    //
    //     const auto request = boost::json::value_to<Dto::ENS::ListQueueRequest>(jv);
    //     log_info << "SQS ListQueues" << (!request.prefix.empty() ? ", prefix: " + request.prefix : "");
    //
    //     const auto repo = Database::RepositoryFactory::instance().ensRepository();
    //     const std::vector<Database::Entity::ENS::Queue> queues = repo->listQueues(request.prefix, request.pageSize, request.pageIndex, request.sortColumn);
    //     log_info << "Got queue list, count: " << queues.size();
    //
    //     Dto::ENS::ListQueueResponse response;
    //     response.queues = Dto::ENS::EnsMapper::toDto(queues);
    //     response.total = repo->countQueues();
    //
    //     return EnsServer::JsonResponse(req, status::ok, response.toJson());
    // }
    //
    // static response<string_body> handleListMessages(const request<string_body> &req) {
    //
    //     Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-messages");
    //
    //     if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);
    //
    //     boost::json::value jv;
    //     if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;
    //
    //     const auto request = boost::json::value_to<Dto::ENS::ListMessagesRequest>(jv);
    //     log_info << "SQS ListMessages, queueErn: " << request.queueErn;
    //
    //     const auto repo = Database::RepositoryFactory::instance().ensRepository();
    //     const std::vector<Database::Entity::ENS::Message> messages = repo->listMessages(request.queueErn, request.pageSize, request.pageIndex, request.sortColumn);
    //     log_info << "Got message list, count: " << messages.size();
    //
    //     Dto::ENS::ListMessagesResponse response;
    //     response.messages = Dto::ENS::EnsMapper::toDto(messages);
    //     response.total = repo->countMessages(request.queueErn);
    //
    //     return EnsServer::JsonResponse(req, status::ok, response.toJson());
    // }
    //
    // static response<string_body> handleSendMessage(const request<string_body> &req) {
    //
    //     Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "send-message");
    //
    //     const auto auth = authenticate(req);
    //     if (!auth.user.has_value()) return unauthorized(req, auth);
    //
    //     boost::json::value jv;
    //     if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;
    //
    //     const auto request = boost::json::value_to<Dto::ENS::SendMessageRequest>(jv);
    //     log_info << "SQS SendMessage queueErn: " << request.queueErn;
    //
    //     const std::string messageId = Core::UuidUtils::CreateRandomUuid();
    //     const std::string ern = Core::createEnsMessageErn(auth.user.value().accountId, messageId);
    //     std::map<std::string, Database::Entity::COM::Variant> attributes;
    //     for (const auto &[key, variant]: request.attributes) {
    //         attributes[key] = Dto::ENS::EnsMapper::toEntity(variant);
    //     }
    //     const auto repo = Database::RepositoryFactory::instance().ensRepository();
    //     const Database::Entity::ENS::Message message = repo->sendMessage(messageId, ern, request.queueErn, request.body, attributes, Database::Entity::ENS::MessagePriorityFromString(request.priority));
    //
    //     Dto::ENS::SendMessageResponse response;
    //     response.messageId = message.messageId;
    //     response.md5Body = message.md5Body;
    //     response.md5Attributes = message.md5Attributes;
    //
    //     return EnsServer::JsonResponse(req, status::ok, response.toJson());
    // }
    //
    // static response<string_body> handleReceiveMessage(const request<string_body> &req) {
    //
    //     Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "receive-messages");
    //
    //     if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);
    //
    //     boost::json::value jv;
    //     if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;
    //
    //     const auto request = boost::json::value_to<Dto::ENS::ReceiveMessagesRequest>(jv);
    //     log_info << "SQS ReceiveMessages ern: " << request.queueErn;
    //
    //     const auto repo = Database::RepositoryFactory::instance().ensRepository();
    //     std::vector<Database::Entity::ENS::Message> messages = repo->receiveMessages(request.queueErn, request.maxCount, request.waitTime);
    //
    //     Dto::ENS::ReceiveMessagesResponse response;
    //     response.messages = Dto::ENS::EnsMapper::toDto(messages);
    //     response.total = repo->countMessages(request.queueErn);
    //
    //     log_info << "SQS ReceiveMessages ern: " << request.queueErn << ", count: " << response.messages.size() << ", total: " << response.total;
    //
    //     return EnsServer::JsonResponse(req, status::ok, response.toJson());
    // }
    //
    // static response<string_body> handleDeleteMessage(const request<string_body> &req) {
    //
    //     Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-message");
    //
    //     if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);
    //
    //     boost::json::value jv;
    //     if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;
    //
    //     const auto request = boost::json::value_to<Dto::ENS::DeleteMessageRequest>(jv);
    //     log_info << "SQS DeleteMessage receiptHandle: " << request.receiptHandle;
    //
    //     const auto repo = Database::RepositoryFactory::instance().ensRepository();
    //     repo->deleteMessage(request.receiptHandle);
    //
    //     return EnsServer::JsonResponse(req, status::ok);
    // }
    //
    // static response<string_body> handlePurgeQueue(const request<string_body> &req) {
    //
    //     Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "purge-queue");
    //
    //     if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);
    //
    //     boost::json::value jv;
    //     if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;
    //
    //     const auto request = boost::json::value_to<Dto::ENS::PurgeQueueRequest>(jv);
    //     log_info << "ENS PurgeQueue ern: " << request.ern;
    //
    //     const auto repo = Database::RepositoryFactory::instance().ensRepository();
    //     repo->purgeQueue(request.ern);
    //
    //     return EnsServer::JsonResponse(req, status::ok);
    // }
    //
    // static response<string_body> handlePurgeAllQueues(const request<string_body> &req) {
    //
    //     Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "purge-all-queues");
    //
    //     if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);
    //
    //     boost::json::value jv;
    //     if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;
    //
    //     const auto request = boost::json::value_to<Dto::ENS::PurgeAllQueuesRequest>(jv);
    //     log_info << "ENS PurgeAllQueues";
    //
    //     const auto repo = Database::RepositoryFactory::instance().ensRepository();
    //     repo->purgeAllQueues(request.region, request.accountId);
    //
    //     return EnsServer::JsonResponse(req, status::ok);
    // }
    //
    // static response<string_body> handleGetMessageCount(const request<string_body> &req) {
    //
    //     Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-message-count");
    //
    //     if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);
    //
    //     boost::json::value jv;
    //     if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;
    //
    //     const auto request = boost::json::value_to<Dto::ENS::GetMessageCountRequest>(jv);
    //     log_info << "ENS GetMessageCount, ern: " << request.queueErn;
    //
    //     const auto repo = Database::RepositoryFactory::instance().ensRepository();
    //     const std::optional<Database::Entity::ENS::Queue> queue = repo->findQueueByErn(request.queueErn);
    //     if (!queue.has_value()) {
    //         return EnsServer::ErrorResponse(req, status::not_found, "Queue not found, ern: " + request.queueErn);
    //     }
    //
    //     Dto::ENS::GetMessageCountResponse response;
    //     response.ern = request.queueErn;
    //     response.available = queue->available;
    //     response.delayed = queue->delayed;
    //     response.invisible = queue->invisible;
    //     response.total = queue->available + queue->delayed + queue->invisible;
    //     return EnsServer::JsonResponse(req, status::ok, response.toJson());
    // }
    //
    // static response<string_body> handleGetQueueMetadata(const request<string_body> &req) {
    //
    //     Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-queue-metadata");
    //
    //     if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);
    //
    //     boost::json::value jv;
    //     if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;
    //
    //     const auto request = boost::json::value_to<Dto::ENS::GetQueueMetadataRequest>(jv);
    //     log_info << "ENS GetQueueMetadata, ern: " << request.ern;
    //
    //     const auto repo = Database::RepositoryFactory::instance().ensRepository();
    //     const std::optional<Database::Entity::ENS::Queue> queue = repo->findQueueByErn(request.ern);
    //     if (!queue.has_value()) {
    //         return EnsServer::ErrorResponse(req, status::not_found, "Queue not found, ern: " + request.ern);
    //     }
    //
    //     Dto::ENS::GetQueueMetadataResponse response;
    //     response.region = queue->region;
    //     response.accountId = Core::accountIdFromErn(queue->ern);
    //     response.owner = queue->owner;
    //     // Queues aren't namespace-scoped yet (no such field on Database::Entity::ENS::Queue), so
    //     // this is always empty for now rather than reporting something fabricated.
    //     response.nameSpace = "";
    //     response.name = queue->name;
    //     response.ern = queue->ern;
    //     response.size = queue->size;
    //     // "Total" across every message state, unlike get-message-count's breakdown into the three
    //     // individual available/delayed/invisible counts.
    //     response.messages = queue->available + queue->delayed + queue->invisible;
    //     return EnsServer::JsonResponse(req, status::ok, response.toJson());
    // }
    //
    // static response<string_body> handleGetMessageAttribute(const request<string_body> &req) {
    //
    //     Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-message-attribute");
    //
    //     if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);
    //
    //     boost::json::value jv;
    //     if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;
    //
    //     const auto request = boost::json::value_to<Dto::ENS::GetMessageAttributeRequest>(jv);
    //     log_info << "ENS GetMessageAttribute, messageId: " << request.messageId << ", name: " << request.name;
    //
    //     const auto repo = Database::RepositoryFactory::instance().ensRepository();
    //     const std::optional<Database::Entity::ENS::Message> message = repo->findMessageByName(request.messageId);
    //     if (!message.has_value()) {
    //         return EnsServer::ErrorResponse(req, status::not_found, "Message not found, messageId: " + request.messageId);
    //     }
    //
    //     const auto attribute = message->attributes.find(request.name);
    //     if (attribute == message->attributes.end()) {
    //         return EnsServer::ErrorResponse(req, status::not_found, "Attribute not found, name: " + request.name);
    //     }
    //
    //     Dto::ENS::GetMessageAttributeResponse response;
    //     response.messageId = request.messageId;
    //     response.name = request.name;
    //     response.value = Dto::ENS::EnsMapper::toDto(attribute->second);
    //
    //     return EnsServer::JsonResponse(req, status::ok, response.toJson());
    // }
    //
    // static response<string_body> handleGetMessageMetadata(const request<string_body> &req) {
    //
    //     Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-message-metadata");
    //
    //     if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);
    //
    //     boost::json::value jv;
    //     if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;
    //
    //     const auto request = boost::json::value_to<Dto::ENS::GetMessageMetadataRequest>(jv);
    //     log_info << "ENS GetMessageMetadata, messageId: " << request.messageId;
    //
    //     const auto repo = Database::RepositoryFactory::instance().ensRepository();
    //     const std::optional<Database::Entity::ENS::Message> message = repo->findMessageByName(request.messageId);
    //     if (!message.has_value()) {
    //         return EnsServer::ErrorResponse(req, status::not_found, "Message not found, messageId: " + request.messageId);
    //     }
    //
    //     Dto::ENS::GetMessageMetadataResponse response;
    //     response.messageId = message->messageId;
    //     response.queueErn = message->queueErn;
    //     response.receiptHandle = message->receiptHandle;
    //     response.status = Database::Entity::ENS::MessageStatusToString(message->status);
    //     response.priority = Database::Entity::ENS::MessagePriorityToString(message->priority);
    //     response.size = message->size;
    //     response.receivedCount = message->receivedCount;
    //     response.visibilityTimeout = message->visibilityTimeout;
    //     response.contentType = message->contentType;
    //     response.md5Body = message->md5Body;
    //     response.md5Attributes = message->md5Attributes;
    //     response.created = Core::DateTimeUtils::ToISO8601(message->created);
    //     response.modified = Core::DateTimeUtils::ToISO8601(message->modified);
    //     return EnsServer::JsonResponse(req, status::ok, response.toJson());
    // }
    //
    // static response<string_body> handleGetQueueAttributes(const request<string_body> &req) {
    //
    //     Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-metadata");
    //
    //     if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);
    //
    //     boost::json::value jv;
    //     if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;
    //
    //     log_info << "ENS GetQueueAttributes url=" << Core::GetStringValue(jv, "QueueUrl");
    //
    //     const boost::json::object body{
    //             {"Attributes", boost::json::object{}},
    //             {"ResponseMetadata", boost::json::object{{"RequestId", EnsServer::RequestId()}}}
    //     };
    //     return EnsServer::JsonResponse(req, status::ok, boost::json::serialize(body));
    // }
    //
    // static response<string_body> handleSetQueueAttributes(const request<string_body> &req) {
    //
    //     Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "add-metadata");
    //
    //     if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);
    //
    //     boost::json::value jv;
    //     if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;
    //
    //     log_info << "ENS SetQueueAttributes url=" << Core::GetStringValue(jv, "QueueUrl");
    //
    //     const boost::json::object body{
    //             {"ResponseMetadata", boost::json::object{{"RequestId", EnsServer::RequestId()}}}
    //     };
    //     return EnsServer::JsonResponse(req, status::ok, boost::json::serialize(body));
    // }
    //
    // static response<string_body> handleAddQueueTag(const request<string_body> &req) {
    //
    //     Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "add-queue-tag");
    //
    //     if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);
    //
    //     boost::json::value jv;
    //     if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;
    //
    //     const auto [ern, key, value] = boost::json::value_to<Dto::ENS::AddQueueTagRequest>(jv);
    //     log_info << "ENS AddQueueTag, ern: " << ern << ", key: " << key;
    //
    //     const auto repo = Database::RepositoryFactory::instance().ensRepository();
    //     std::optional<Database::Entity::ENS::Queue> queue = repo->findQueueByErn(ern);
    //     if (!queue.has_value()) {
    //         return EnsServer::ErrorResponse(req, status::not_found, "Queue not found, ern: " + ern);
    //     }
    //     queue->tags[key] = value;
    //     queue = repo->upsertQueue(queue.value());
    //
    //     return EnsServer::JsonResponse(req, status::ok);
    // }
    //
    // static response<string_body> handleSetQueueTag(const request<string_body> &req) {
    //
    //     Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "set-queue-tag");
    //
    //     if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);
    //
    //     boost::json::value jv;
    //     if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;
    //
    //     const auto [ern, key, value] = boost::json::value_to<Dto::ENS::AddQueueTagRequest>(jv);
    //     log_info << "ENS SetQueueTag, ern: " << ern << ", key: " << key;
    //
    //     const auto repo = Database::RepositoryFactory::instance().ensRepository();
    //     std::optional<Database::Entity::ENS::Queue> queue = repo->findQueueByErn(ern);
    //     if (!queue.has_value()) {
    //         return EnsServer::ErrorResponse(req, status::not_found, "Queue not found, ern: " + ern);
    //     }
    //     if (!queue.value().tags.contains(key)) {
    //         return EnsServer::ErrorResponse(req, status::not_found, "Tag not found, key: " + key);
    //     }
    //     queue->tags[key] = value;
    //     queue = repo->upsertQueue(queue.value());
    //
    //     return EnsServer::JsonResponse(req, status::ok);
    // }
    //
    // static response<string_body> handleDeleteQueueTag(const request<string_body> &req) {
    //
    //     Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-queue-tag");
    //
    //     if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);
    //
    //     boost::json::value jv;
    //     if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;
    //
    //     const auto [ern, key] = boost::json::value_to<Dto::ENS::DeleteQueueTagRequest>(jv);
    //     log_info << "ENS DeleteQueueTag, ern: " << ern << ", key: " << key;
    //
    //     const auto repo = Database::RepositoryFactory::instance().ensRepository();
    //     std::optional<Database::Entity::ENS::Queue> queue = repo->findQueueByErn(ern);
    //     if (!queue.has_value()) {
    //         return EnsServer::ErrorResponse(req, status::not_found, "Queue not found, ern: " + ern);
    //     }
    //     queue->tags.erase(key);
    //     queue = repo->upsertQueue(queue.value());
    //
    //     return EnsServer::JsonResponse(req, status::ok);
    // }

    // ── Request dispatcher ───────────────────────────────────────────────────

    namespace {
        // Commands the ENS service accepts via the "x-euclid-action" header.
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
            SetQueueTag,
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
        if (action == "set-queue-tag") return Command::SetQueueTag;
        if (action == "delete-queue-tag") return Command::DeleteQueueTag;
        if (action == "get-metrics") return Command::GetMetrics;
        return Command::Unknown;
    }

    static response<string_body> dispatch(const request<string_body> &req) {

        const auto action = std::string(req["x-euclid-action"]);
        if (action.empty()) {
            return EnsServer::ErrorResponse(req, status::bad_request, "Missing x-euclid-action header");
        }
        log_debug << "ENS action=" << action;

        switch (commandFromString(action)) {
            //
            // case Command::CreateQueue:
            //     return handleCreateQueue(req);
            //
            // case Command::DeleteQueue:
            //     return handleDeleteQueue(req);
            //
            // case Command::GetQueueErn:
            //     return handleGetQueueErn(req);
            //
            // case Command::ListQueues:
            //     return handleListQueues(req);
            //
            // case Command::ListMessages:
            //     return handleListMessages(req);
            //
            // case Command::SendMessage:
            //     return handleSendMessage(req);
            //
            // case Command::ReceiveMessages:
            //     return handleReceiveMessage(req);
            //
            // case Command::DeleteMessage:
            //     return handleDeleteMessage(req);
            //
            // case Command::PurgeQueue:
            //     return handlePurgeQueue(req);
            //
            // case Command::PurgeAllQueues:
            //     return handlePurgeAllQueues(req);
            //
            // case Command::GetMetadata:
            //     return handleGetQueueAttributes(req);
            //
            // case Command::AddMetadata:
            //     return handleSetQueueAttributes(req);
            //
            // case Command::GetMessageCount:
            //     return handleGetMessageCount(req);
            //
            // case Command::GetQueueMetadata:
            //     return handleGetQueueMetadata(req);
            //
            // case Command::GetMessageAttribute:
            //     return handleGetMessageAttribute(req);
            //
            // case Command::GetMessageMetadata:
            //     return handleGetMessageMetadata(req);
            //
            // case Command::AddQueueTag:
            //     return handleAddQueueTag(req);
            //
            // case Command::SetQueueTag:
            //     return handleSetQueueTag(req);
            //
            // case Command::DeleteQueueTag:
            //     return handleDeleteQueueTag(req);

            case Command::GetMetrics:
                return EnsServer::MetricsResponse(req);

            case Command::Unknown:
            default:
                log_warning << "Unknown action: " << action;
                return EnsServer::ErrorResponse(req, status::not_found, "Action not implemented: " + action);
        }
    }

    // ── EnsServer ────────────────────────────────────────────────────────────

    EnsServer::EnsServer(std::string socketPath, const int threads) : HttpActionServer("ENS", std::move(socketPath), threads) {
        // auto &scheduler = Core::Scheduler::instance();
        // scheduler.Start();
        // _resetMessagesTaskId = scheduler.SchedulePeriodic("queues-reset-expired-messages", [] {
        //                                                       Database::RepositoryFactory::instance().ensRepository()->resetExpiredMessages();
        //                                                   },
        //                                                   std::chrono::seconds(30));
    }

    EnsServer::~EnsServer() {
        Core::Scheduler::instance().Cancel(_resetMessagesTaskId);
    }

    response<string_body> EnsServer::Dispatch(const request<string_body> &req) {
        return dispatch(req);
    }

}// namespace Euclid::ENS