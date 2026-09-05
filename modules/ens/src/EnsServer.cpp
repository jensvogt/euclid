// Euclid includes
#include <EnsServer.h>

#include "euclid/core/HttpUtils.h"

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
        constexpr auto kServiceTimer = "ens-service-time";
        constexpr auto kServiceCounter = "ens-service-count";

        // Message and byte volume per topic, the counterpart of EQS's per-queue counters and read
        // the same way round: "sent" is what was published into a topic, "received" is what came
        // back out of it - here that means what was handed to its subscriptions, since a topic has
        // no consumers of its own to pull from it. One publish to three subscriptions therefore
        // counts once as sent and three times as received, which is exactly the fan-out factor.
        //
        // Labelled by ERN rather than by topic name, for the same reason EQS labels by queue ERN:
        // it is what these call sites already hold, and it stays unique across accounts and
        // namespaces.
        constexpr auto kTopicLabel = "topic";
        constexpr auto kMessagesSent = "ens-messages-sent";
        constexpr auto kMessagesReceived = "ens-messages-received";
        constexpr auto kBytesSent = "ens-bytes-sent";
        constexpr auto kBytesReceived = "ens-bytes-received";

        // One event carrying its own count rather than one signal per message: sigMetricCounter
        // sums amounts into the same rate metric sigMetricRate counts occurrences in.
        void recordMessages(const char *messageMetric, const char *byteMetric, const std::string &topicErn, const long messages, const long bytes) {
            if (messages <= 0) return;
            auto &bus = Core::Monitoring::MetricEventBus::instance();
            bus.sigMetricCounter(messageMetric, kTopicLabel, topicErn, static_cast<double>(messages));
            if (bytes > 0) bus.sigMetricCounter(byteMetric, kTopicLabel, topicErn, static_cast<double>(bytes));
        }
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
        topic.ern = Core::createEnsTopicErn(auth.user->accountId, ns, request.name);
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
        const std::vector<Database::Entity::ENS::Topic> topics = repo->listTopics(auth.user->accountId, ns, request.prefix, request.pageSize, request.pageIndex, request.sortColumn, request.sortDirection);
        log_info << "ENS ListTopics count, count: " << topics.size();

        Dto::ENS::ListTopicsResponse response;
        response.topics = Dto::ENS::EnsMapper::toDto(topics);
        response.total = repo->countTopics(auth.user->accountId, ns, request.prefix);

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
        const std::vector<Database::Entity::ENS::Message> messages = repo->listMessages(request.topicErn, request.pageSize, request.pageIndex, request.sortColumn, request.sortDirection);
        log_info << "Got message list, count: " << messages.size();

        Dto::ENS::ListMessagesResponse response;
        response.messages = Dto::ENS::EnsMapper::toDto(messages);
        response.total = repo->countMessages(request.topicErn);

        return EnsServer::JsonResponse(req, status::ok, response.toJson());
    }

    // Creates a message on topicErn and fans it out to every SQS-type subscription of that topic
    // - one EventBus event per subscription, so an eqs instance (any one of them, via the claim
    // mechanism) creates the corresponding queue message. Other subscription types are ignored.
    // Shared by handlePublishMessage (a direct client publish) and
    // handleObjectPublishedNotification (an ESM object-created notification arriving via an
    // SNS-type ESM subscription), so a topic behaves the same regardless of who published to it.
    static Database::Entity::ENS::Message publishToTopic(const std::string &topicErn, const std::string &body,
                                                         const std::map<std::string, Dto::COM::Variant> &attributes,
                                                         const std::string &accountId) {

        const std::string messageId = Core::UuidUtils::CreateRandomUuid();
        const std::string ern = Core::createEnsMessageErn(accountId, messageId);
        std::map<std::string, Database::Entity::COM::Variant> entityAttributes;
        for (const auto &[key, variant]: attributes) {
            entityAttributes[key] = Dto::ENS::EnsMapper::toEntity(variant);
        }
        const auto repo = Database::RepositoryFactory::instance().ensRepository();
        const Database::Entity::ENS::Message message = repo->publishMessage(messageId, ern, topicErn, body, entityAttributes);

        // Counted here rather than in handlePublishMessage, so that a message arriving from an
        // ESM object notification counts the same as one a client published - both reach a topic
        // only through this function.
        recordMessages(kMessagesSent, kBytesSent, topicErn, 1, message.size);

        boost::json::object attributesJson;
        for (const auto &[key, variant]: attributes) {
            attributesJson[key] = boost::json::value_from(variant);
        }
        long delivered = 0;
        for (const auto &subscription: repo->listSubscriptionsBySourceErn(topicErn)) {
            if (subscription.type != "SQS") continue;
            const boost::json::value payload = {
                    {"body", body},
                    {"attributes", attributesJson},
            };
            Database::EventBus::instance().Publish("ens.message.published", payload, "ens",
                                                   {.targetErn = subscription.targetErn,
                                                    .sourceErn = topicErn,
                                                    .messageId = message.messageId});
            ++delivered;
        }
        recordMessages(kMessagesReceived, kBytesReceived, topicErn, delivered, delivered * message.size);

        return message;
    }

    static response<string_body> handlePublishMessage(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "publish-message");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EnsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::ENS::PublishMessageRequest>(jv);
        log_info << "ENS PublishMessage topicErn: " << request.ern;

        // Checked before anything is stored. Without this a publish to a topic that does not exist
        // was accepted and kept: the message landed in the collection carrying whatever the caller
        // called the topic, no subscription matched it, and nothing ever said so - which is how a
        // misspelled or unresolved topic name becomes a pile of rows belonging to no topic. The
        // EventBus path below has always checked; this is the same check on the client path.
        const auto repo = Database::RepositoryFactory::instance().ensRepository();
        if (!repo->findTopicByErn(request.ern).has_value()) {
            return EnsServer::ErrorResponse(req, status::not_found, "Topic not found, ern: " + request.ern);
        }

        const auto message = publishToTopic(request.ern, request.body, request.attributes, auth.user->accountId);

        Dto::ENS::PublishMessageResponse response;
        response.messageId = message.messageId;

        return EnsServer::JsonResponse(req, status::ok, response.toJson());
    }

    // ── EventBus ─────────────────────────────────────────────────────────────
    // Consumer side of ESM's SNS-type bucket subscriptions: an object-created notification
    // targeting an ENS topic (EsmServer::notifyBucketSubscriptions, event "esm.subscription.publication") is
    // published as a regular topic message here, so it flows through the topic's own subscription
    // fan-out (publishToTopic above) exactly like a client-published message would.

    static bool handleObjectPublishedNotification(const Database::EventEnvelope &envelope) {

        // Envelope first, payload second - see EqsServer::handleSubscriptionDelivery. An event
        // published by the previous binary carries the target in its payload, and is still being
        // consumed by this one until the backlog drains.
        const auto targetErn = !envelope.targetErn.empty() ? envelope.targetErn : Core::GetStringValue(envelope.payload, "targetErn");
        const auto body = Core::GetStringValue(envelope.payload, "body");

        const auto repo = Database::RepositoryFactory::instance().ensRepository();
        if (!repo->findTopicByErn(targetErn).has_value()) {
            log_warning << "ENS EventBus object-published notification: target topic not found, ern: " << targetErn;
            return true;// ack - topic is gone, nothing to retry
        }

        const auto message = publishToTopic(targetErn, body, {}, Core::accountIdFromErn(targetErn));

        log_info << "ENS created message from ESM object-published notification, source: " << envelope.sourceModule << ", targetErn: " << targetErn
                  << ", messageId: " << message.messageId;
        return true;
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
        log_info << "ENS GetMessageCount, ern: " << request.ern;

        const auto repo = Database::RepositoryFactory::instance().ensRepository();
        const std::optional<Database::Entity::ENS::Topic> queue = repo->findTopicByErn(request.ern);
        if (!queue.has_value()) {
            return EnsServer::ErrorResponse(req, status::not_found, "Topic not found, ern: " + request.ern);
        }

        Dto::ENS::GetMessageCountResponse response;
        response.ern = request.ern;
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
    //    //    //     response.created = Core::DateTimeUtils::ToISO8601(message->created);
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

    // Resolves the resource names clients send into the full ERNs every handler below expects.
    //
    // A client's configuration names a queue or a topic; an ERN additionally carries the region,
    // account and namespace, which are properties of the caller's session rather than of the
    // request. Resolving here rather than in each client means one implementation instead of one
    // per language, and it bounds what a name can reach: a name always resolves inside the
    // caller's own namespace. A full ERN is passed through untouched, so every client written
    // before this keeps working and cross-namespace work stays possible.
    //
    // Installed once, in the constructor, and applied by Core::HttpActionServer::ParseJsonBody to
    // every body this process parses - so a handler added later gets it without having to know.
    static void installErnResolver() {
        Core::HttpActionServer::SetRequestRewriter([](const auto &req, boost::json::value &body) {
            if (!body.is_object()) return;
            auto &obj = body.as_object();

            // The header, which authenticate() has already checked is one this caller may use -
            // every handler authenticates before it parses. It is optional though (see
            // CheckScope), and an empty account would build an ERN with a hole in it that matches
            // nothing, so a single-account installation's configured id stands in for it.
            auto accountId = std::string(req["x-euclid-account-id"]);
            if (accountId.empty()) {
                // has() first: getArray() throws on a missing key. Only when exactly one account
                // is configured - with several there is no way to tell which was meant, and a
                // guess would resolve the name into somebody else's account.
                if (const auto &cfg = Core::Configuration::instance(); cfg.has("euclid.account-ids")) {
                    if (const auto configured = cfg.getArray<std::string>("euclid.account-ids"); configured.size() == 1) {
                        accountId = configured.front();
                    }
                }
            }
            const auto nameSpace = std::string(req["x-euclid-namespace"]);
            const auto action = std::string(req["x-euclid-action"]);

            auto resolve = [&](const char *field, const char *service, const char *type) {
                const auto it = obj.find(field);
                if (it == obj.end() || !it->value().is_string()) return;
                const auto resolved = Core::resolveErn(service, type, accountId, nameSpace, std::string(it->value().as_string()));
                it->value() = resolved;
            };

            // Unambiguous wherever they appear: a topicErn is always a topic, and the queueErn
            // a subscription names is always an EQS queue.
            resolve("topicErn", "ens", "topic");
            resolve("queueErn", "eqs", "queue");

            // A bare "ern" too, unconditionally. It names a topic in most actions and an object,
            // message or subscription in the rest - but that distinction does not matter here,
            // because a full ERN is always passed through untouched and only a bare value is ever
            // resolved. A bare value in one of those other actions was never valid anyway, so the
            // worst this can do is fail with a different message than it used to.
            //
            // Deliberately not an action list: the wire field a DTO serialises to is not always
            // the name of its C++ member (PublishMessageRequest::topicErn is sent as "ern"), so
            // any list keyed on one would be wrong for the actions where the two disagree - which
            // is exactly how publish-message was missed.
            resolve("ern", "ens", "topic");

            // subscribe names two resources of different kinds: the topic published from, and the
            // EQS queue delivered into. ENS only supports SQS targets, so the target's type is not
            // in question here.
            resolve("sourceErn", "ens", "topic");
            resolve("targetErn", "eqs", "queue");
        });
    }

    EnsServer::EnsServer(std::string socketPath, const int threads) : HttpActionServer("ENS", std::move(socketPath), threads) {
        installErnResolver();
        Database::EventBus::instance().Subscribe("ens", "esm.subscription.publication", handleObjectPublishedNotification);
        Database::EventBus::instance().Start("ens");
    }

    EnsServer::~EnsServer() {
        Core::Scheduler::instance().Cancel(_resetMessagesTaskId);
    }

    response<string_body> EnsServer::Dispatch(const request<string_body> &req) {
        return dispatch(req);
    }

}// namespace Euclid::ENS