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
        constexpr auto kServiceTimer = "topic-service-time";
        constexpr auto kServiceCounter = "topic-service-count";
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

    // ── Action handlers ──────────────────────────────────────────────────────
    // Each handler parses whatever fields it needs out of the JSON request body.
    // Return a fully formed HTTP response.

    static response<string_body> handleCreateTopic(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "create-topic");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ENS::CreateTopicRequest>(jv);
        const auto ns = std::string(req["x-euclid-namespace"]);

        Database::Entity::ENS::Topic topic;
        topic.accountId = auth.user->accountId;
        topic.nameSpace = ns;
        topic.name = request.name;
        topic.ern = Core::createEnsTopicErn(auth.user->accountId, request.name);
        topic.maxMessageLength = request.maxMessageLength;
        topic.region = auth.user->region;
        topic.owner = auth.user->userId;

        const auto saved = Database::RepositoryFactory::instance().ensRepository()->upsertTopic(topic);

        Dto::ENS::CreateTopicResponse response;
        response.name = saved.name;
        response.ern = saved.ern;
        return EnsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleDeleteTopic(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-topic");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = Dto::ENS::DeleteTopicRequest::fromJson(req.body());
        log_info << "ENS DeleteTopic, ern: " << request.ern;

        Database::RepositoryFactory::instance().ensRepository()->deleteTopicByErn(request.ern);

        return EnsServer::JsonResponse(req, status::ok);
    }


    static response<string_body> handleGetTopicErn(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-topic-ern");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ENS::GetTopicErnRequest>(jv);
        log_info << "ENS GetTopicErn, name: " << request.name;

        const std::optional<Database::Entity::ENS::Topic> topic = Database::RepositoryFactory::instance().ensRepository()->findTopicByName(request.name);
        log_debug << "Got ENS topic ern, name: " << request.name << ", ern: " << (topic.has_value() ? topic->ern : "(none)");

        if (!topic.has_value()) {
            return EnsServer::ErrorResponse(req, status::not_found, "Topic not found, name: " + request.name);
        }

        Dto::ENS::GetTopicErnResponse response;
        response.name = topic->name;
        response.ern = topic->ern;

        return EnsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleListTopics(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-topics");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ENS::ListTopicsRequest>(jv);
        log_info << "ENS ListTopics" << (!request.prefix.empty() ? ", prefix: " + request.prefix : "");

        const auto ns = std::string(req["x-euclid-namespace"]);
        const auto repo = Database::RepositoryFactory::instance().ensRepository();
        const std::vector<Database::Entity::ENS::Topic> topics = repo->listTopics(auth.user->accountId, ns, request.prefix, request.pageSize, request.pageIndex, request.sortColumn);
        log_info << "ENS ListTopics count, count: " << topics.size();

        Dto::ENS::ListTopicsResponse response;
        response.topics = Dto::ENS::EnsMapper::toDto(topics);
        response.total = repo->countTopics(auth.user->accountId, ns);

        return EnsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleListMessages(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-messages");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ENS::ListMessagesRequest>(jv);
        log_debug << "ENS ListMessages, topicErn: " << request.topicErn;

        const auto repo = Database::RepositoryFactory::instance().ensRepository();
        const std::vector<Database::Entity::ENS::Message> messages = repo->listMessages(request.topicErn, request.pageSize, request.pageIndex, request.sortColumn);
        log_info << "Got message list, count: " << messages.size();

        Dto::ENS::ListMessagesResponse response;
        response.messages = Dto::ENS::EnsMapper::toDto(messages);
        response.total = repo->countMessages(request.topicErn);

        return EnsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handlePublishMessage(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "publish-message");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ENS::PublishMessageRequest>(jv);
        log_info << "ENS PublishMessage topicErn: " << request.topicErn;

        const std::string messageId = Core::UuidUtils::CreateRandomUuid();
        const std::string ern = Core::createEnsMessageErn(auth.user.value().accountId, messageId);
        std::map<std::string, Database::Entity::COM::Variant> attributes;
        for (const auto &[key, variant]: request.attributes) {
            attributes[key] = Dto::ENS::EnsMapper::toEntity(variant);
        }
        const auto repo = Database::RepositoryFactory::instance().ensRepository();
        const Database::Entity::ENS::Message message = repo->publishMessage(messageId, ern, request.topicErn, request.body, attributes);

        // Fan out to every SQS-type subscription of this topic - one EventBus event per
        // subscription, so an eqs instance (any one of them, via the claim mechanism) creates the
        // corresponding queue message. Other subscription types (none exist yet) are ignored.
        boost::json::object attributesJson;
        for (const auto &[key, variant]: request.attributes) {
            attributesJson[key] = boost::json::value_from(variant);
        }
        for (const auto &subscription: repo->listSubscriptionsBySourceErn(request.topicErn)) {
            if (subscription.type != "SQS") continue;
            const boost::json::value payload = {
                    {"messageId", message.messageId},
                    {"sourceErn", request.topicErn},
                    {"targetErn", subscription.targetErn},
                    {"body", request.body},
                    {"attributes", attributesJson},
            };
            Database::EventBus::instance().Publish("ens.message.published", payload, "ens");
        }

        Dto::ENS::PublishMessageResponse response;
        response.messageId = message.messageId;
        response.md5Body = message.md5Body;
        response.md5Attributes = message.md5Attributes;

        return EnsServer::JsonResponse(req, status::ok, response.toJson());
    }

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
    //     log_info << "ENS ReceiveMessages ern: " << request.queueErn;
    //
    //     const auto repo = Database::RepositoryFactory::instance().ensRepository();
    //     std::vector<Database::Entity::ENS::Message> messages = repo->receiveMessages(request.queueErn, request.maxCount, request.waitTime);
    //
    //     Dto::ENS::ReceiveMessagesResponse response;
    //     response.messages = Dto::ENS::EnsMapper::toDto(messages);
    //     response.total = repo->countMessages(request.queueErn);
    //
    //     log_info << "ENS ReceiveMessages ern: " << request.queueErn << ", count: " << response.messages.size() << ", total: " << response.total;
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
    //     log_info << "ENS DeleteMessage receiptHandle: " << request.receiptHandle;
    //
    //     const auto repo = Database::RepositoryFactory::instance().ensRepository();
    //     repo->deleteMessage(request.receiptHandle);
    //
    //     return EnsServer::JsonResponse(req, status::ok);
    // }

    static response<string_body> handlePurgeTopic(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "purge-topic");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ENS::PurgeTopicRequest>(jv);
        log_info << "ENS PurgeTopic ern: " << request.ern;

        const auto repo = Database::RepositoryFactory::instance().ensRepository();
        repo->purgeTopic(request.ern);

        return EnsServer::JsonResponse(req, status::ok);
    }

    static response<string_body> handlePurgeAllTopics(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "purge-all-topics");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ENS::PurgeAllTopicsRequest>(jv);
        log_info << "ENS PurgeAllTopics";

        const auto repo = Database::RepositoryFactory::instance().ensRepository();
        repo->purgeAllTopics(request.region, request.accountId, request.nameSpace);

        return EnsServer::JsonResponse(req, status::ok);
    }

    static response<string_body> handleGetMessageCount(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-message-count");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ENS::GetMessageCountRequest>(jv);
        log_info << "ENS GetMessageCount, ern: " << request.topicErn;

        const auto repo = Database::RepositoryFactory::instance().ensRepository();
        const std::optional<Database::Entity::ENS::Topic> queue = repo->findTopicByErn(request.topicErn);
        if (!queue.has_value()) {
            return EnsServer::ErrorResponse(req, status::not_found, "Topic not found, ern: " + request.topicErn);
        }

        Dto::ENS::GetMessageCountResponse response;
        response.ern = request.topicErn;
        response.available = queue->available;
        response.send = queue->send;
        response.resend = queue->resend;
        return EnsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleGetTopicMetadata(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-topic-metadata");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ENS::GetTopicMetadataRequest>(jv);
        log_info << "ENS GetTopicMetadata, ern: " << request.ern;

        const auto repo = Database::RepositoryFactory::instance().ensRepository();
        const std::optional<Database::Entity::ENS::Topic> topic = repo->findTopicByErn(request.ern);
        if (!topic.has_value()) {
            return EnsServer::ErrorResponse(req, status::not_found, "Queue not found, ern: " + request.ern);
        }

        Dto::ENS::GetTopicMetadataResponse response;
        response.region = topic->region;
        response.accountId = Core::accountIdFromErn(topic->ern);
        response.owner = topic->owner;
        response.nameSpace = topic->nameSpace;
        response.name = topic->name;
        response.ern = topic->ern;
        response.size = topic->size;
        response.messages = topic->available + topic->send + topic->send;
        return EnsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleGetMessageAttribute(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-message-attribute");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ENS::GetMessageAttributeRequest>(jv);
        log_info << "ENS GetMessageAttribute, messageId: " << request.messageId << ", key: " << request.key;

        // Get the message
        const auto repo = Database::RepositoryFactory::instance().ensRepository();
        const std::optional<Database::Entity::ENS::Message> message = repo->findMessageById(request.messageId);
        if (!message.has_value()) {
            return EnsServer::ErrorResponse(req, status::not_found, "Message not found, messageId: " + request.messageId);
        }

        // Check attribute
        const auto attribute = message->attributes.find(request.key);
        if (attribute == message->attributes.end()) {
            return EnsServer::ErrorResponse(req, status::not_found, "Attribute not found, messageId: " + request.messageId + ", key: " + request.key);
        }

        // Return the attribute
        Dto::ENS::GetMessageAttributeResponse response;
        response.messageId = request.messageId;
        response.key = request.key;
        response.value = Dto::ENS::EnsMapper::toDto(attribute->second);

        return EnsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleSetMessageAttribute(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "set-message-attribute");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ENS::SetMessageAttributeRequest>(jv);
        log_info << "ENS SetMessageAttribute, messageId: " << request.messageId << ", key: " << request.key;

        // Get the message
        const auto repo = Database::RepositoryFactory::instance().ensRepository();
        std::optional<Database::Entity::ENS::Message> message = repo->findMessageById(request.messageId);
        if (!message.has_value()) {
            return EnsServer::ErrorResponse(req, status::not_found, "Message not found, messageId: " + request.messageId);
        }

        // Set attribute and update in database
        message->attributes[request.key] = Dto::ENS::EnsMapper::toEntity(request.value);
        repo->upsertMessage(message.value());

        // Return the attribute
        Dto::ENS::GetMessageAttributeResponse response;
        response.messageId = request.messageId;
        response.key = request.key;
        response.value = Dto::ENS::EnsMapper::toDto(message->attributes[request.key]);

        return EnsServer::JsonResponse(req, status::ok, response.toJson());
    }

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
    static response<string_body> handleAddTopicTag(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "add-topic-tag");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;

        const auto [ern, key, value] = boost::json::value_to<Dto::ENS::AddTopicTagRequest>(jv);
        log_info << "ENS AddTopicTag, ern: " << ern << ", key: " << key;

        const auto repo = Database::RepositoryFactory::instance().ensRepository();
        std::optional<Database::Entity::ENS::Topic> topic = repo->findTopicByErn(ern);
        if (!topic.has_value()) {
            return EnsServer::ErrorResponse(req, status::not_found, "Topic not found, ern: " + ern);
        }
        topic->tags[key] = value;
        topic = repo->upsertTopic(topic.value());

        return EnsServer::JsonResponse(req, status::ok);
    }

    static response<string_body> handleSetTopicTag(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "set-topic-tag");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;

        const auto [ern, key, value] = boost::json::value_to<Dto::ENS::AddTopicTagRequest>(jv);
        log_info << "ENS SetTopicTag, ern: " << ern << ", key: " << key;

        const auto repo = Database::RepositoryFactory::instance().ensRepository();
        std::optional<Database::Entity::ENS::Topic> topic = repo->findTopicByErn(ern);
        if (!topic.has_value()) {
            return EnsServer::ErrorResponse(req, status::not_found, "Topic not found, ern: " + ern);
        }
        if (!topic.value().tags.contains(key)) {
            return EnsServer::ErrorResponse(req, status::not_found, "Tag not found, key: " + key);
        }
        topic->tags[key] = value;
        topic = repo->upsertTopic(topic.value());

        return EnsServer::JsonResponse(req, status::ok);
    }

    static response<string_body> handleDeleteTopicTag(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-topic-tag");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;

        const auto [ern, key] = boost::json::value_to<Dto::ENS::DeleteTopicTagRequest>(jv);
        log_info << "ENS DeleteTopicTag, ern: " << ern << ", key: " << key;

        const auto repo = Database::RepositoryFactory::instance().ensRepository();
        std::optional<Database::Entity::ENS::Topic> topic = repo->findTopicByErn(ern);
        if (!topic.has_value()) {
            return EnsServer::ErrorResponse(req, status::not_found, "Topic not found, ern: " + ern);
        }
        topic->tags.erase(key);
        topic = repo->upsertTopic(topic.value());

        return EnsServer::JsonResponse(req, status::ok);
    }

    namespace {
        bool isEnsTopicErn(const std::string &ern) {
            return ern.starts_with("ern:ens:") && ern.find(":topic:") != std::string::npos;
        }

        bool isEqsQueueErn(const std::string &ern) {
            return ern.starts_with("ern:eqs:") && ern.find(":queue:") != std::string::npos;
        }
    }// namespace

    static response<string_body> handleSubscribe(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "subscribe");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ENS::SubscribeRequest>(jv);
        log_info << "ENS Subscribe, sourceErn: " << request.sourceErn << ", type: " << request.type << ", targetErn: " << request.targetErn;

        if (request.type != "SQS") {
            return EnsServer::ErrorResponse(req, status::bad_request, "Unsupported subscription type (only SQS is supported for now): " + request.type);
        }
        if (!isEnsTopicErn(request.sourceErn)) {
            return EnsServer::ErrorResponse(req, status::bad_request, "sourceErn is not an ENS topic ERN: " + request.sourceErn);
        }
        if (!isEqsQueueErn(request.targetErn)) {
            return EnsServer::ErrorResponse(req, status::bad_request, "targetErn is not an EQS queue ERN: " + request.targetErn);
        }

        const auto repo = Database::RepositoryFactory::instance().ensRepository();
        if (!repo->findTopicByErn(request.sourceErn).has_value()) {
            return EnsServer::ErrorResponse(req, status::not_found, "Topic not found, ern: " + request.sourceErn);
        }
        if (!Database::RepositoryFactory::instance().eqsRepository()->findQueueByErn(request.targetErn).has_value()) {
            return EnsServer::ErrorResponse(req, status::not_found, "Queue not found, ern: " + request.targetErn);
        }

        // Derived from the (sourceErn, type, targetErn) triple rather than randomly generated, so
        // that re-subscribing the same triple upserts the same document with a stable ERN instead
        // of silently reassigning its identity on every call.
        const auto subscriptionId = Core::CryptoUtils::md5Sum(request.sourceErn + ":" + request.type + ":" + request.targetErn);

        Database::Entity::ENS::Subscription subscription;
        subscription.accountId = auth.user->accountId;
        subscription.nameSpace = std::string(req["x-euclid-namespace"]);
        subscription.region = auth.user->region;
        subscription.owner = auth.user->userId;
        subscription.ern = Core::createEnsSubscriptionErn(auth.user->accountId, subscriptionId);
        subscription.sourceErn = request.sourceErn;
        subscription.type = request.type;
        subscription.targetErn = request.targetErn;

        const auto saved = repo->upsertSubscription(subscription);

        Dto::ENS::SubscribeResponse response;
        response.ern = saved.ern;
        response.sourceErn = saved.sourceErn;
        response.type = saved.type;
        response.targetErn = saved.targetErn;

        return EnsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleUnsubscribe(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "unsubscribe");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = Dto::ENS::UnsubscribeRequest::fromJson(req.body());
        log_info << "ENS Unsubscribe, ern: " << request.ern;

        Database::RepositoryFactory::instance().ensRepository()->deleteSubscriptionByErn(request.ern);

        return EnsServer::JsonResponse(req, status::ok);
    }

    static response<string_body> handleListSubscriptions(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-subscriptions");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ENS::ListSubscriptionsRequest>(jv);
        log_info << "ENS ListSubscriptions, topicErn: " << request.topicErn;

        const auto subscriptions = Database::RepositoryFactory::instance().ensRepository()->listSubscriptionsBySourceErn(request.topicErn);

        Dto::ENS::ListSubscriptionsResponse response;
        response.subscriptions = Dto::ENS::EnsMapper::toDto(subscriptions);
        response.total = static_cast<long>(subscriptions.size());

        return EnsServer::JsonResponse(req, status::ok, response.toJson());
    }

    // ── Request dispatcher ───────────────────────────────────────────────────

    namespace {
        // Commands the ENS service accepts via the "x-euclid-action" header.
        enum class Command {
            Unknown,
            CreateTopic,
            DeleteTopic,
            GetTopicErn,
            GetMessageCount,
            GetQueueMetadata,
            GetMessageAttribute,
            SetMessageAttribute,
            GetMessageMetadata,
            ListTopics,
            ListMessages,
            PublishMessage,
            ReceiveMessages,
            DeleteMessage,
            PurgeTopic,
            PurgeAllTopics,
            GetMetadata,
            AddMetadata,
            AddTopicTag,
            SetTopicTag,
            DeleteTopicTag,
            Subscribe,
            Unsubscribe,
            ListSubscriptions,
            GetMetrics
        };
    }

    static Command commandFromString(const std::string &action) {
        if (action == "create-topic") return Command::CreateTopic;
        if (action == "get-topic-ern") return Command::GetTopicErn;
        if (action == "list-topics") return Command::ListTopics;
        if (action == "purge-topic") return Command::PurgeTopic;
        if (action == "purge-all-topics") return Command::PurgeAllTopics;
        if (action == "publish-message") return Command::PublishMessage;
        if (action == "delete-topic") return Command::DeleteTopic;
        if (action == "list-messages") return Command::ListMessages;
        if (action == "get-message-count") return Command::GetMessageCount;
        if (action == "get-message-attribute") return Command::GetMessageAttribute;
        if (action == "set-message-attribute") return Command::SetMessageAttribute;
        if (action == "get-topic-metadata") return Command::GetMetadata;
        if (action == "add-topic-tag") return Command::AddTopicTag;
        if (action == "set-topic-tag") return Command::SetTopicTag;
        if (action == "delete-topic-tag") return Command::DeleteTopicTag;
        if (action == "subscribe") return Command::Subscribe;
        if (action == "unsubscribe") return Command::Unsubscribe;
        if (action == "list-subscriptions") return Command::ListSubscriptions;
        // if (action == "delete-message") return Command::DeleteMessage;
        // if (action == "purge-queue") return Command::PurgeQueue;
        // if (action == "purge-all-queues") return Command::PurgeAllQueues;
        // if (action == "get-message-count") return Command::GetMessageCount;
        // if (action == "get-queue-metadata") return Command::GetQueueMetadata;
        // if (action == "get-message-metadata") return Command::GetMessageMetadata;
        // if (action == "add-metadata") return Command::AddMetadata;
        // if (action == "add-queue-tag") return Command::AddQueueTag;
        // if (action == "set-queue-tag") return Command::SetQueueTag;
        // if (action == "delete-queue-tag") return Command::DeleteQueueTag;
        // if (action == "get-metrics") return Command::GetMetrics;
        return Command::Unknown;
    }

    static response<string_body> dispatch(const request<string_body> &req) {

        const auto action = std::string(req["x-euclid-action"]);
        if (action.empty()) {
            return EnsServer::ErrorResponse(req, status::bad_request, "Missing x-euclid-action header");
        }
        log_debug << "ENS action=" << action;

        switch (commandFromString(action)) {

            case Command::CreateTopic:
                return handleCreateTopic(req);

            case Command::DeleteTopic:
                return handleDeleteTopic(req);

            case Command::GetTopicErn:
                return handleGetTopicErn(req);

            case Command::ListTopics:
                return handleListTopics(req);

            case Command::ListMessages:
                return handleListMessages(req);

            case Command::PublishMessage:
                return handlePublishMessage(req);

            case Command::PurgeTopic:
                return handlePurgeTopic(req);

            case Command::PurgeAllTopics:
                return handlePurgeAllTopics(req);

            case Command::GetMetadata:
                return handleGetTopicMetadata(req);

            case Command::GetMessageCount:
                return handleGetMessageCount(req);
            //
            // case Command::GetQueueMetadata:
            //     return handleGetQueueMetadata(req);
            //
            case Command::GetMessageAttribute:
                return handleGetMessageAttribute(req);

            case Command::SetMessageAttribute:
                return handleSetMessageAttribute(req);

            // case Command::GetMessageMetadata:
            //     return handleGetMessageMetadata(req);
            //
            case Command::AddTopicTag:
                return handleAddTopicTag(req);

            case Command::SetTopicTag:
                return handleSetTopicTag(req);

            case Command::DeleteTopicTag:
                return handleDeleteTopicTag(req);

            case Command::Subscribe:
                return handleSubscribe(req);

            case Command::Unsubscribe:
                return handleUnsubscribe(req);

            case Command::ListSubscriptions:
                return handleListSubscriptions(req);

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