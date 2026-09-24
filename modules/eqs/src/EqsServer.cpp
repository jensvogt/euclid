// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <thread>

// Euclid includes
#include <EqsServer.h>

#include <boost/asio/query.hpp>
#include <boost/asio/query.hpp>

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
        constexpr auto kServiceTimer = "eqs-service-time";
        constexpr auto kServiceCounter = "eqs-service-count";

        // The most messages one send-message-batch accepts.
        //
        // A bound rather than a guess: the batch arrives as one request body, is built into one
        // insert and is answered with one list of ids, so the number decides how much memory a
        // single caller can ask this module to hold. A hundred is where the per-message cost of a
        // round trip has already stopped mattering - insert_many measured a thousand documents at
        // 29 ms against 13,621 ms one at a time - and is still a request an operator can read.
        //
        // Floored at one rather than trusted: a misread or missing setting must not mean "accept
        // nothing", which would take send-message-batch out of service without saying so.
        long MaxBatchSize() {
            constexpr long kDefaultMaxBatchSize = 100;
            return std::max<long>(1, Core::Configuration::instance().getOr<long>("euclid.modules.eqs.max-batch-size", kDefaultMaxBatchSize));
        }

        // How many worker threads may sit in a receive-messages wait at once, always leaving one
        // over. receiveMessages() holds its thread for the whole waitTime, so without this cap
        // enough waiting consumers leave nothing to answer a send, a delete or a create - and
        // those do not fail, they queue behind the waits and are served once one ends, often just
        // after the caller gave up on them. See Core::LongPollSlots.
        Core::LongPollSlots longPollSlots;

        // Message and byte volume per queue, named after the actions that move them: "sent" is
        // what went into a queue, "received" is what a consumer took back out. One queue is one
        // label, so a deployment's traffic can be read per queue rather than only in total.
        //
        // Labelled by ERN rather than by queue name: it is the identifier every one of these call
        // sites already has without a second lookup, and the only one that stays unique across
        // accounts and namespaces.
        constexpr auto kQueueLabel = "queue";
        constexpr auto kMessagesSent = "eqs-messages-sent";
        constexpr auto kMessagesReceived = "eqs-messages-received";
        constexpr auto kBytesSent = "eqs-bytes-sent";
        constexpr auto kBytesReceived = "eqs-bytes-received";

        // A batch is recorded as one event carrying its own count, rather than one signal per
        // message: sigMetricCounter sums amounts into the same rate metric sigMetricRate counts
        // occurrences in, so a receive of ten messages costs one call and reads as ten.
        void recordMessagesSent(const std::string &queueErn, const long messages, const long bytes) {
            if (messages <= 0) return;
            auto &bus = Core::Monitoring::MetricEventBus::instance();
            bus.sigMetricCounter(kMessagesSent, kQueueLabel, queueErn, static_cast<double>(messages));
            if (bytes > 0) bus.sigMetricCounter(kBytesSent, kQueueLabel, queueErn, static_cast<double>(bytes));
        }

        void recordMessagesReceived(const std::string &queueErn, const long messages, const long bytes) {
            if (messages <= 0) return;
            auto &bus = Core::Monitoring::MetricEventBus::instance();
            bus.sigMetricCounter(kMessagesReceived, kQueueLabel, queueErn, static_cast<double>(messages));
            if (bytes > 0) bus.sigMetricCounter(kBytesReceived, kQueueLabel, queueErn, static_cast<double>(bytes));
        }
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

    // Whether this caller was given this queue - the counterpart of ESM's bucket check, and the
    // same reasoning: which queue a request is about is named in its body, so only a handler can
    // ask. A caller with no resource grants at all, which is every human, is unaffected.
    static std::optional<response<string_body> > denyUngrantedQueue(const request<string_body> &req, const AuthResult &auth, const std::string &queueErn) {

        if (!auth.user.has_value()) return std::nullopt;

        // See ESM's denyUngrantedBucket.

        if (auto refusal = EqsServer::AuthorizeResource(req, queueErn)) {
            log_warning << "EQS resource denied, userId: " << auth.user->userId << ", queueErn: " << queueErn;
            return refusal;
        }
        return std::nullopt;
    }

    // Whether the queue a destructive request names is the caller's own plumbing - see
    // Queue::isInternalPlumbingOf() for why that is let through where a deployed queue is not.
    //
    // The flags are read from the *stored* queue and never from the request, so a caller cannot
    // claim somebody else's queue is its own plumbing. A queue that is not there is not exempt
    // either: the check then falls through to the resource grant rather than past it, so a narrow
    // grant cannot be probed for which queues exist.
    static bool isOwnInternalQueue(const std::optional<Database::Entity::EQS::Queue> &queue, const AuthResult &auth) {
        return queue.has_value() && auth.user.has_value() && queue->isInternalPlumbingOf(auth.user->userId);
    }

    // Fills in the caller identity every response DTO inherits from BaseDto and serialises as a
    // nested "metadata" object. Resolved from the authenticated user rather than from anything the
    // request said it was. The request ID that correlates this response with its request travels
    // as the "x-euclid-request-id" header instead (set centrally in HttpActionServer::JsonResponse).
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
        const auto ns = std::string(req["x-euclid-namespace"]);

        Database::Entity::EQS::Queue dlqSaved;
        if (!request.dlqName.empty()) {
            Database::Entity::EQS::Queue dlQueue;
            dlQueue.accountId = auth.user->accountId;
            dlQueue.nameSpace = ns;
            dlQueue.name = request.dlqName;
            dlQueue.ern = Core::createEqsQueueErn(auth.user->accountId, ns, request.dlqName);
            dlQueue.visibility = request.visibility;
            dlQueue.maxMessageLength = request.maxMessageLength;
            dlQueue.maxReceiveCount = request.maxRetries;
            dlQueue.region = auth.user->region;
            dlQueue.owner = auth.user->userId;
            dlQueue.delay = request.delay;
            // Hidden exactly as its source queue is. A dead letter queue is part of the plumbing
            // the source queue already is - a listener's delivery queue and the place its failures
            // go are one arrangement, not two - so offering one in list-queues while hiding the
            // other shows half a mechanism and invites somebody to act on it. A caller that wants
            // its queues visible gets a visible dead letter queue by the same token.
            dlQueue.internal = request.internal;
            dlQueue.created = std::chrono::system_clock::now();
            dlQueue.modified = std::chrono::system_clock::now();

            dlqSaved = Database::RepositoryFactory::instance().eqsRepository()->upsertQueue(dlQueue);
        }

        Database::Entity::EQS::Queue queue;
        queue.accountId = auth.user->accountId;
        queue.nameSpace = ns;
        queue.name = request.name;
        queue.ern = Core::createEqsQueueErn(auth.user->accountId, ns, request.name);
        queue.visibility = request.visibility;
        queue.maxMessageLength = request.maxMessageLength;
        queue.maxReceiveCount = request.maxRetries;
        queue.region = auth.user->region;
        queue.owner = auth.user->userId;
        queue.deadLetterQueueErn = dlqSaved.ern;
        // Kept out of list-queues and the queue count when asked for; see Queue::internal. The
        // dead letter queue created above takes the same flag - see there.
        queue.internal = request.internal;
        queue.delay = request.delay;
        queue.created = std::chrono::system_clock::now();
        queue.modified = std::chrono::system_clock::now();

        const auto saved = Database::RepositoryFactory::instance().eqsRepository()->upsertQueue(queue);

        Dto::EQS::CreateQueueResponse response;
        response.name = saved.name;
        response.ern = saved.ern;
        applyMetadata(response, *auth.user);
        return EqsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleDeleteQueue(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-queue");

        // Hoisted out of the `if` it used to be declared in: the resource check below needs it.
        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EQS::DeleteQueueRequest>(jv);
        log_info << "EQS DeleteQueue, ern: " << request.ern;

        const auto repo = Database::RepositoryFactory::instance().eqsRepository();

        // Deleting is destructive and was not resource-checked at all until 2026-09-13, so a
        // principal granted one queue could remove any of them. Its own delivery plumbing is the
        // exception, because that queue is not something a resource list can name.
        if (const auto existing = repo->findQueueByErn(request.ern); !isOwnInternalQueue(existing, auth)) {
            if (const auto denied = denyUngrantedQueue(req, auth, request.ern)) return *denied;
        }

        repo->deleteQueueByErn(request.ern);

        // Anything still queued to be delivered into it has nowhere to go now. Discarded here so
        // the backlog never forms, rather than being rediscovered one event at a time by
        // handleSubscriptionDelivery once the queue is already gone.
        if (const auto discarded = Database::EventBus::instance().DiscardDeliveries(request.ern); discarded > 0) {
            log_info << "EQS DeleteQueue discarded undeliverable events, ern: " << request.ern << ", count: " << discarded;
        }

        return EqsServer::JsonResponse(req, status::ok);
    }

    static response<string_body> handleGetQueueErn(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-queue-ern");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EQS::GetQueueErnRequest>(jv);
        log_info << "EQS GetQueueErn, name: " << request.name;

        // Resolved against the caller's own account and namespace, the same pair create-queue
        // built the ERN from - a bare name means "my queue of that name", and cannot reach into
        // another account's or another namespace's queue of the same name.
        const auto ns = std::string(req["x-euclid-namespace"]);
        const std::optional<Database::Entity::EQS::Queue> queue = Database::RepositoryFactory::instance().eqsRepository()->findQueueByName(auth.user->accountId, ns, request.name);
        log_debug << "Got EQS queue, name: " << request.name << ", ern: " << (queue.has_value() ? queue->ern : "(none)");

        if (!queue.has_value()) {
            return EqsServer::ErrorResponse(req, status::not_found, "Queue not found, name: " + request.name);
        }

        Dto::EQS::GetQueueErnResponse response;
        response.ern = queue->ern;

        applyMetadata(response, *auth.user);
        return EqsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleGetQueue(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-queue");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EQS::GetQueueRequest>(jv);
        if (request.ern.empty() && request.name.empty()) {
            return EqsServer::ErrorResponse(req, status::bad_request, "Either ern or name is required");
        }

        // A name is resolved against the caller's own account and namespace, the pair create-queue
        // built the ERN from, so it means "my queue of that name"; an ERN names one queue in the
        // installation and is taken as given.
        const auto ns = std::string(req["x-euclid-namespace"]);
        const auto repo = Database::RepositoryFactory::instance().eqsRepository();
        const auto queue = request.ern.empty()
                                   ? repo->findQueueByName(auth.user->accountId, ns, request.name)
                                   : repo->findQueueByErn(request.ern);

        if (!queue.has_value()) {
            return EqsServer::ErrorResponse(req, status::not_found,
                                            request.ern.empty() ? "Queue not found, name: " + request.name
                                                                : "Queue not found, ern: " + request.ern);
        }

        if (const auto denied = denyUngrantedQueue(req, auth, queue->ern)) return *denied;

        log_debug << "EQS get queue, ern: " << queue->ern;

        Dto::EQS::GetQueueResponse response;
        response.queue = Dto::EQS::EqsMapper::toDto(*queue);

        applyMetadata(response, *auth.user);
        return EqsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleGetMessage(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-message");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EQS::GetMessageRequest>(jv);
        if (request.messageId.empty()) {
            return EqsServer::ErrorResponse(req, status::bad_request, "messageId is required");
        }

        const auto repo = Database::RepositoryFactory::instance().eqsRepository();
        const auto message = repo->findMessageByName(request.messageId);
        if (!message.has_value()) {
            return EqsServer::ErrorResponse(req, status::not_found, "Message not found, messageId: " + request.messageId);
        }

        // Guarded by the queue the message is in rather than by the message: a message is not a
        // resource anybody is granted, and reading one is reading from its queue.
        if (const auto denied = denyUngrantedQueue(req, auth, message->queueErn)) return *denied;

        log_debug << "EQS get message, messageId: " << request.messageId;

        Dto::EQS::GetMessageResponse response;
        response.message = Dto::EQS::EqsMapper::toDto(*message);

        applyMetadata(response, *auth.user);
        return EqsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleListQueues(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-queues");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EQS::ListQueueRequest>(jv);
        log_info << "EQS ListQueues" << (!request.prefix.empty() ? ", prefix: " + request.prefix : "");

        const auto ns = std::string(req["x-euclid-namespace"]);
        const auto repo = Database::RepositoryFactory::instance().eqsRepository();

        // euclid's own queues are plumbing - the bucket queue behind a listener, and whatever else
        // the modules give themselves - and a user has no business seeing them among their own.
        // An administrator may ask, because for them the installation itself is the subject; the
        // ask is silently refused for everyone else rather than answered with a 403, since the
        // request is otherwise perfectly valid and the queues they asked about are none of theirs.
        const auto includeInternal = request.includeInternal
                                     && Database::IsEamAdmin(*Database::RepositoryFactory::instance().eamRepository(), auth.user->userId);

        const std::vector<Database::Entity::EQS::Queue> queues = repo->listQueues(auth.user->accountId, ns, request.prefix, request.pageSize, request.pageIndex, request.sortColumn, request.sortDirection, includeInternal);
        log_info << "Got queue list, count: " << queues.size();

        Dto::EQS::ListQueueResponse response;
        response.queues = Dto::EQS::EqsMapper::toDto(queues);
        response.total = repo->countQueues(auth.user->accountId, ns, request.prefix, includeInternal);

        applyMetadata(response, *auth.user);
        return EqsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleListMessages(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-messages");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EQS::ListMessagesRequest>(jv);
        log_info << "EQS ListMessages, queueErn: " << request.queueErn;

        const auto repo = Database::RepositoryFactory::instance().eqsRepository();
        const std::vector<Database::Entity::EQS::Message> messages = repo->listMessages(request.queueErn, request.pageSize, request.pageIndex, request.sortColumn, request.sortDirection);
        log_info << "Got message list, count: " << messages.size();

        Dto::EQS::ListMessagesResponse response;
        response.messages = Dto::EQS::EqsMapper::toDto(messages);
        response.total = repo->countMessages(request.queueErn);

        applyMetadata(response, *auth.user);
        return EqsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleSendMessage(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "send-message");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EQS::SendMessageRequest>(jv);
        log_info << "EQS SendMessage queueErn: " << request.ern;

        const auto repo = Database::RepositoryFactory::instance().eqsRepository();
        std::optional<Database::Entity::EQS::Queue> queue = repo->findQueueByErn(request.ern);
        if (!queue.has_value()) {
            return EqsServer::ErrorResponse(req, status::bad_request, "Queue does not exist");
        }
        if (const auto denied = denyUngrantedQueue(req, auth, request.ern)) return *denied;

        // The body alone, which is what Message::size counts and what get-queue-metadata reports,
        // so the limit an operator sets is measured in the same units as the numbers they compare
        // it against. Attributes travel alongside and are not counted. Checked after the grant, so
        // a caller with no business here learns that and not the size of somebody else's queue.
        //
        // A queue carrying no limit of its own is measured against the default rather than refused:
        // a create-queue that omitted the field used to store zero, and every queue made that way
        // would otherwise stop accepting anything the moment this check arrived.
        const auto maxMessageLength = Database::Entity::EQS::EffectiveMaxMessageLength(queue->maxMessageLength);
        if (const auto length = static_cast<long>(request.body.size()); length > maxMessageLength) {
            return EqsServer::ErrorResponse(req, status::bad_request,
                                            "message is " + std::to_string(length) + " bytes, and this queue accepts " +
                                                    std::to_string(maxMessageLength));
        }

        const std::string messageId = Core::UuidUtils::CreateRandomUuid();
        const std::string ern = Core::createEqsMessageErn(auth.user.value().accountId, messageId);

        // Attributes
        std::map<std::string, Database::Entity::COM::Variant> attributes;
        for (const auto &[key, variant]: request.attributes) {
            attributes[key] = Dto::EQS::EqsMapper::toEntity(variant);
        }

        // The queue's default, unless the caller names one. Initialised here and not merely
        // declared: a scoped enum with no initialiser holds an indeterminate value, and reading it
        // is undefined - which shows up as a priority that looks plausible, is stable enough to
        // seem deliberate, and has nothing to do with what anybody asked for.
        Database::Entity::EQS::MessagePriority priority = queue->priority;
        if (!request.priority.empty()) {
            // Refused rather than absorbed. A stored value that cannot be read falls back to
            // MEDIUM, because a message nobody can parse is still a message somebody is waiting
            // for - but a caller who wrote "low" and got MEDIUM was never told, and every send
            // after it is wrong in the same invisible way.
            const auto requested = Database::Entity::EQS::TryMessagePriorityFromString(request.priority);
            if (!requested.has_value()) {
                return EqsServer::ErrorResponse(req, status::bad_request, R"(priority must be "LOW", "MEDIUM" or "HIGH", not ")" + request.priority + R"(")");
            }
            priority = *requested;
        }

        // Said separately from the resolved value, because "the caller asked for MEDIUM" and "the
        // caller asked for nothing and the queue is MEDIUM" are the same answer and quite
        // different problems.
        log_debug << "EQS SendMessage priority, requested: " << (request.priority.empty() ? "(none)" : request.priority)
                  << ", queue default: " << Database::Entity::EQS::MessagePriorityToString(queue->priority)
                  << ", using: " << Database::Entity::EQS::MessagePriorityToString(priority);

        // Create message
        // The envelope the caller was carrying, passed on. A service that received a message and
        // is sending the next one is the middle of a chain, and whatever was travelling - a
        // correlation id most of all - has to keep travelling, or it identifies only the first hop.
        std::map<std::string, Database::Entity::COM::Variant> systemAttributes;
        for (const auto &[key, variant]: request.systemAttributes) {
            systemAttributes[key] = Dto::EQS::EqsMapper::toEntity(variant);
        }

        const Database::Entity::EQS::Message message = repo->sendMessage(messageId, ern, request.ern, request.body, attributes, systemAttributes, priority);
        recordMessagesSent(request.ern, 1, message.size);

        // Published, not pushed: a client that wants to know about new messages subscribes to
        // this event type through EES like it would to any other, and the bus decides who gets it
        // - so a listener can be told live over its websocket, or have the events kept for it
        // while it is away, without EQS knowing which. accountId and region come from the queue
        // because a message does not carry them, and they are what scopes the event to an
        // account; queueName is here so a subscriber can filter on the queue it cares about
        // rather than receiving every queue's traffic.
        Database::EventBus::instance().Publish(
                "eqs.message.sent",
                boost::json::value{
                        {"ern", message.ern},
                        {"queueErn", message.queueErn},
                        {"queueName", queue->name},
                        {"messageId", message.messageId},
                        {"size", message.size},
                        {"accountId", queue->accountId},
                        {"region", queue->region},
                },
                "eqs");

        Dto::EQS::SendMessageResponse response;
        response.messageId = message.messageId;

        applyMetadata(response, *auth.user);
        return EqsServer::JsonResponse(req, status::ok, response.toJson());
    }

    // Puts several messages on one queue in a single call.
    //
    // The saving is not mainly the round trip. A send is a signature to verify, an authorization to
    // decide, a queue to resolve and an insert to wait for; a batch does the first three once for
    // the whole lot, and the fourth becomes one insert_many, which measured a thousand documents at
    // 29 ms against 13,621 ms one at a time.
    //
    // A message that cannot be sent is reported against its position and the rest still go. That is
    // not what delete-objects does - a key naming nothing is skipped there and only counted - and
    // the difference is deliberate: a delete that skipped something removed what was already gone,
    // while a send that skipped something dropped it. A producer holding "97 of 100" and no way to
    // learn which three would have to send all hundred again, and duplicate ninety-seven of them.
    static response<string_body> handleSendMessageBatch(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "send-message-batch");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        // Checked on the raw value rather than after parsing, because the parsed request cannot
        // tell "messages was absent" from "messages was an empty array" - and those are different
        // mistakes. A shape that is wrong fails the whole request, which is the house rule.
        if (!jv.is_object()) return EqsServer::ErrorResponse(req, status::bad_request, "Expected a JSON object body");
        const auto *messagesField = jv.as_object().if_contains("messages");
        if (messagesField == nullptr || !messagesField->is_array()) {
            return EqsServer::ErrorResponse(req, status::bad_request, "messages must be an array of objects");
        }

        const auto request = boost::json::value_to<Dto::EQS::SendMessageBatchRequest>(jv);
        const auto asked = static_cast<long>(request.messages.size());
        log_info << "EQS SendMessageBatch queueErn: " << request.ern << ", messages: " << asked;

        if (const auto refusal = Database::Entity::EQS::BatchRefusal(asked, MaxBatchSize()); !refusal.empty()) {
            return EqsServer::ErrorResponse(req, status::bad_request, refusal);
        }

        const auto repo = Database::RepositoryFactory::instance().eqsRepository();
        std::optional<Database::Entity::EQS::Queue> queue = repo->findQueueByErn(request.ern);
        if (!queue.has_value()) {
            return EqsServer::ErrorResponse(req, status::bad_request, "Queue does not exist");
        }

        // Before anything is looked at message by message, so a caller with no business here
        // learns that and not how big somebody else's queue accepts.
        if (const auto denied = denyUngrantedQueue(req, auth, request.ern)) return *denied;

        const auto maxMessageLength = Database::Entity::EQS::EffectiveMaxMessageLength(queue->maxMessageLength);

        std::vector<Database::IEqsRepository::MessageDraft> drafts;
        std::vector<Dto::EQS::SendMessageBatchFailure> failed;
        drafts.reserve(request.messages.size());

        for (std::size_t i = 0; i < request.messages.size(); ++i) {
            const auto &entry = request.messages[i];

            // The same two checks a single send makes, from the same place - see BatchEntryRefusal,
            // which exists so the batch cannot drift into accepting what send-message refuses, or
            // refusing what it accepts.
            if (auto refusal = Database::Entity::EQS::BatchEntryRefusal(static_cast<long>(entry.body.size()),
                                                                        maxMessageLength, entry.priority);
                !refusal.empty()) {
                failed.push_back(Dto::EQS::SendMessageBatchFailure{.index = static_cast<long>(i), .reason = std::move(refusal)});
                continue;
            }

            Database::IEqsRepository::MessageDraft draft;
            draft.messageId = Core::UuidUtils::CreateRandomUuid();
            draft.ern = Core::createEqsMessageErn(auth.user->accountId, draft.messageId);
            draft.body = entry.body;

            for (const auto &[key, variant]: entry.attributes) {
                draft.attributes[key] = Dto::EQS::EqsMapper::toEntity(variant);
            }
            // The envelope the caller was carrying, passed on per message - see handleSendMessage.
            for (const auto &[key, variant]: entry.systemAttributes) {
                draft.systemAttributes[key] = Dto::EQS::EqsMapper::toEntity(variant);
            }

            // The queue's own unless this message names one. BatchEntryRefusal has already refused
            // anything unparsable, so the value here is known good.
            draft.priority = queue->priority;
            if (!entry.priority.empty()) {
                draft.priority = Database::Entity::EQS::TryMessagePriorityFromString(entry.priority).value();
            }

            drafts.push_back(std::move(draft));
        }

        Dto::EQS::SendMessageBatchResponse response;
        response.ern = request.ern;
        response.asked = asked;
        response.failed = std::move(failed);

        // Nothing acceptable is not an error: every message was answered, each with its own reason,
        // and the caller has exactly what it needs. Refusing the request as well would take the
        // reasons away with it.
        if (!drafts.empty()) {

            const auto sent = repo->sendMessages(request.ern, drafts);
            if (sent.empty()) {
                return EqsServer::ErrorResponse(req, status::internal_server_error, "Could not send the batch");
            }

            long bytes = 0;
            std::vector<std::pair<boost::json::value, Database::EventBus::Delivery> > events;
            events.reserve(sent.size());

            for (const auto &message: sent) {
                response.messageIds.push_back(message.messageId);
                bytes += message.size;
                events.emplace_back(boost::json::value{
                                            {"ern", message.ern},
                                            {"queueErn", message.queueErn},
                                            {"queueName", queue->name},
                                            {"messageId", message.messageId},
                                            {"size", message.size},
                                            {"accountId", queue->accountId},
                                            {"region", queue->region},
                                    },
                                    Database::EventBus::Delivery{});
            }

            // Once for the batch, because both already take a count: a series that jumped by one a
            // hundred times would say the same thing about volume and something quite wrong about
            // how many sends there were.
            recordMessagesSent(request.ern, static_cast<long>(sent.size()), bytes);

            // One event per message still - a subscriber watching a queue wants to hear about each
            // message, not about the batch somebody happened to group them into - but published
            // together, which is one insert rather than N. PublishBatch takes the event type once
            // because every event here is the same type.
            Database::EventBus::instance().PublishBatch("eqs.message.sent", events, "eqs");

            response.sent = static_cast<long>(sent.size());
        }

        applyMetadata(response, *auth.user);
        return EqsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleReceiveMessage(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "receive-messages");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EQS::ReceiveMessagesRequest>(jv);
        // At info for a real consumer, at debug for the system polling itself. A receive is worth
        // a line when somebody is waiting for work; the same call made every few seconds by a
        // metric collector or a heartbeat says nothing and would bury everything that does. The
        // caller states which it is - see the gateway's x-euclid-internal - rather than this
        // guessing from a rate.
        const bool internalTraffic = std::string(req["x-euclid-internal"]) == "true";
        if (internalTraffic) {
            log_debug << "EQS ReceiveMessages ern: " << request.ern;
        } else {
            log_info << "EQS ReceiveMessages ern: " << request.ern;
        }

        if (const auto denied = denyUngrantedQueue(req, auth, request.ern)) return *denied;

        const auto repo = Database::RepositoryFactory::instance().eqsRepository();

        // Checked before the long poll, not after: waiting twenty seconds to be told the queue is
        // stopped would be twenty seconds a consumer spends holding a slot for an answer that was
        // already known.
        if (const auto queue = repo->findQueueByErn(request.ern);
            queue.has_value() && queue->status == Database::Entity::EQS::QueueStatus::STOPPED) {
            return EqsServer::ErrorResponse(req, status::conflict, "Queue is stopped, ern: " + request.ern);
        }

        // Without a slot the wait is skipped rather than queued: the queue is checked once and
        // whatever is in it comes back, which is what a long poll returns anyway when the queue
        // stays empty. The consumer asks again; a producer gets a thread meanwhile.
        const auto slot = longPollSlots.acquire();
        std::vector<Database::Entity::EQS::Message> messages =
                repo->receiveMessages(request.ern, request.maxCount, slot.held() ? request.waitTime : 0);

        // Counted per message handed out, so a message received twice (its visibility timeout ran
        // out before it was deleted) counts twice - that redelivery is real traffic, and the gap
        // between this and eqs-messages-sent is what makes it visible.
        long receivedBytes = 0;
        for (const auto &message: messages) receivedBytes += message.size;
        recordMessagesReceived(request.ern, static_cast<long>(messages.size()), receivedBytes);

        Dto::EQS::ReceiveMessagesResponse response;
        response.messages = Dto::EQS::EqsMapper::toDto(messages);
        response.total = repo->countMessages(request.ern);

        if (internalTraffic) {
            log_debug << "EQS ReceiveMessages ern: " << request.ern << ", count: " << response.messages.size() << ", total: " << response.total;
        } else {
            log_info << "EQS ReceiveMessages ern: " << request.ern << ", count: " << response.messages.size() << ", total: " << response.total;
        }

        applyMetadata(response, *auth.user);
        return EqsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleSetVisibility(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "set-visibility");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EQS::SetMessageVisibilityRequest>(jv);
        if (request.messageId.empty()) {
            return EqsServer::ErrorResponse(req, status::bad_request, "Message ID missing");
        }
        // Same bounds as AWS SQS's ChangeMessageVisibility (0s..12h).
        if (request.visibility < 0 || request.visibility > 43200) {
            return EqsServer::ErrorResponse(req, status::bad_request, "Visibility must be between 0 and 43200 seconds");
        }
        log_info << "EQS SetVisibility, messageId: " << request.messageId << ", visibility: " << request.visibility;

        const auto repo = Database::RepositoryFactory::instance().eqsRepository();
        std::optional<Database::Entity::EQS::Message> message = repo->findMessageByName(request.messageId);
        if (!message.has_value()) {
            return EqsServer::ErrorResponse(req, status::not_found, "Message not found, messageId: " + request.messageId);
        }

        // For an in-flight message this restarts the remaining invisibility window from now,
        // matching AWS's "new timeout counted from the time of the request" semantics. For a
        // message that is not yet in flight (AVAILABLE/DELAYED), this just changes the timeout
        // that will apply the next time it is received - a Euclid-specific extension, since AWS
        // only allows ChangeMessageVisibility on a message currently leased via receipt handle.
        message->visibilityTimeout = request.visibility;
        if (message->status == Database::Entity::EQS::MessageStatus::INVISIBLE) {
            message->lastReceived = std::chrono::system_clock::now();
        }
        repo->upsertMessage(message.value());

        return EqsServer::JsonResponse(req, status::ok);
    }

    static response<string_body> handleDeleteMessage(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-message");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EQS::DeleteMessageRequest>(jv);
        const auto repo = Database::RepositoryFactory::instance().eqsRepository();

        if (!request.receiptHandle.empty()) {
            log_info << "EQS DeleteMessage receiptHandle: " << request.receiptHandle;
            repo->deleteMessage(request.receiptHandle);
        } else if (!request.messageId.empty()) {
            log_info << "EQS DeleteMessage messageId: " << request.messageId;
            repo->deleteMessageById(request.messageId);
        } else {
            return EqsServer::ErrorResponse(req, status::bad_request, "Receipt handle or message ID missing");
        }

        return EqsServer::JsonResponse(req, status::ok);
    }

    static response<string_body> handlePurgeQueue(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "purge-queue");

        // Hoisted out of the `if` it used to be declared in: the resource check below needs it.
        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EQS::PurgeQueueRequest>(jv);
        log_info << "EQS PurgeQueue ern: " << request.ern;

        const auto repo = Database::RepositoryFactory::instance().eqsRepository();

        // Emptying a queue loses exactly as much as deleting it, so it is held to the same rule.
        const auto existing = repo->findQueueByErn(request.ern);
        if (!isOwnInternalQueue(existing, auth)) {
            if (const auto denied = denyUngrantedQueue(req, auth, request.ern)) return *denied;
        }

        if (request.async) {
            // Answered before the work is done, the same trade ESM's purge-bucket makes: a queue
            // with a large backlog takes longer to empty than the gateway will wait, so doing it
            // inline hands the caller a timeout while the purge runs on regardless. Detached
            // rather than tracked because there is nothing to resume - a purge interrupted halfway
            // has removed part of the queue, and asking again removes the rest.
            const auto ern = request.ern;
            std::thread([ern] {
                try {
                    log_info << "EQS background purge started, ern: " << ern;
                    Database::RepositoryFactory::instance().eqsRepository()->purgeQueue(ern);
                    log_info << "EQS background purge finished, ern: " << ern;
                } catch (const std::exception &e) {
                    log_error << "EQS background purge failed, ern: " << ern << ", error: " << e.what();
                }
            }).detach();

            // What was there when the purge was accepted, which is the only count anybody can be
            // given: by the time it finishes the number is zero, and by the time this is read
            // something may already have sent more.
            return EqsServer::JsonResponse(req, status::accepted,
                                           boost::json::serialize(boost::json::object{
                                                   {"ern", request.ern},
                                                   {"async", true},
                                                   {"messages", existing.has_value() ? existing->available : 0}}));
        }

        repo->purgeQueue(request.ern);

        return EqsServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::object{
                                               {"ern", request.ern},
                                               {"async", false}}));
    }

    static response<string_body> handleRedriveDlq(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "redrive-dlq");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;
        if (!jv.is_object()) return EqsServer::ErrorResponse(req, status::bad_request, "Expected a JSON object body");

        const auto ern = Core::GetStringValue(jv, "ern");
        const auto targetErn = Core::GetStringValue(jv, "targetErn");
        if (ern.empty()) return EqsServer::ErrorResponse(req, status::bad_request, "ern is required");

        const auto repo = Database::RepositoryFactory::instance().eqsRepository();

        const auto deadLetterQueue = repo->findQueueByErn(ern);
        if (!deadLetterQueue.has_value()) {
            return EqsServer::ErrorResponse(req, status::not_found, "Queue not found, ern: " + ern);
        }

        // Nothing on a queue says "I am a dead letter queue" - the relationship is only ever
        // written the other way round, by the queues that name it. So the question "is this a
        // DLQ" is answered by asking who points at it, and an ordinary queue is refused here
        // rather than being silently redriven into itself or into nothing.
        const auto sourceQueues = repo->listSourceQueues(ern);
        if (sourceQueues.empty()) {
            return EqsServer::ErrorResponse(req, status::bad_request,
                                            "Queue is not a dead letter queue, ern: " + ern
                                            + " - no queue names it as its dead letter queue");
        }

        long moved = 0;
        boost::json::array targets;

        if (!targetErn.empty()) {

            // An explicit target still has to be one of the queues that feed this one. Anything
            // else would be a move, not a redrive, and would put messages somewhere they were
            // never sent.
            const auto named = std::ranges::find_if(sourceQueues, [&targetErn](const auto &queue) { return queue.ern == targetErn; });
            if (named == sourceQueues.end()) {
                return EqsServer::ErrorResponse(req, status::bad_request,
                                                "Target queue does not use this dead letter queue, targetErn: " + targetErn);
            }
            moved = repo->redriveMessages(ern, targetErn, "");
            targets.push_back(boost::json::object{{"queueErn", targetErn}, {"messages", moved}});

        } else if (sourceQueues.size() == 1) {

            // The unambiguous case, and the common one: one queue feeds this dead letter queue,
            // so everything in it came from there - including messages that predate the recording
            // of where they came from.
            moved = repo->redriveMessages(ern, sourceQueues.front().ern, "");
            targets.push_back(boost::json::object{{"queueErn", sourceQueues.front().ern}, {"messages", moved}});

        } else {

            // Several queues share this dead letter queue, so "the original queue" is only
            // answerable per message. Each goes back where it came from; anything with no origin
            // recorded is left alone rather than guessed at, and the answer says how many, so the
            // caller can name a target and deal with them deliberately.
            for (const auto &source: sourceQueues) {
                if (const auto count = repo->redriveMessages(ern, source.ern, source.ern); count > 0) {
                    targets.push_back(boost::json::object{{"queueErn", source.ern}, {"messages", count}});
                    moved += count;
                }
            }
        }

        log_info << "EQS RedriveDlq, ern: " << ern << ", messages: " << moved;

        const auto remaining = repo->countMessages(ern);
        boost::json::object result{
                {"ern", ern},
                {"messages", moved},
                {"remaining", remaining},
                {"targets", targets}};
        if (remaining > 0) {
            result["note"] = "Messages remain in the dead letter queue because no source queue is recorded for them. "
                    "Name one with a target queue to move them.";
        }

        return EqsServer::JsonResponse(req, status::ok, boost::json::serialize(result));
    }

    static response<string_body> handlePurgeAllQueues(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "purge-all-queues");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EQS::PurgeAllQueuesRequest>(jv);
        log_info << "EQS PurgeAllQueues";

        const auto repo = Database::RepositoryFactory::instance().eqsRepository();
        repo->purgeAllQueues(request.region, request.accountId, request.nameSpace);

        return EqsServer::JsonResponse(req, status::ok);
    }

    static response<string_body> handleGetMessageCount(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-message-count");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EQS::GetMessageCountRequest>(jv);
        log_info << "EQS GetMessageCount, ern: " << request.ern;

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
        applyMetadata(response, *auth.user);
        return EqsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleGetQueueMetadata(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-queue-metadata");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EQS::GetQueueMetadataRequest>(jv);
        log_info << "EQS GetQueueMetadata, ern: " << request.ern;

        const auto repo = Database::RepositoryFactory::instance().eqsRepository();
        const std::optional<Database::Entity::EQS::Queue> queue = repo->findQueueByErn(request.ern);
        if (!queue.has_value()) {
            return EqsServer::ErrorResponse(req, status::not_found, "Queue not found, ern: " + request.ern);
        }

        Dto::EQS::GetQueueMetadataResponse response;
        response.region = queue->region;
        response.accountId = Core::accountIdFromErn(queue->ern);
        response.owner = queue->owner;
        // Queues aren't namespace-scoped yet (no such field on Database::Entity::EQS::Queue), so
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

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EQS::GetMessageAttributeRequest>(jv);
        log_info << "EQS GetMessageAttribute, messageId: " << request.messageId << ", name: " << request.name;

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

        applyMetadata(response, *auth.user);
        return EqsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleSetMessageAttribute(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "set-message-attribute");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EQS::SetMessageAttributeRequest>(jv);
        log_info << "EQS SetMessageAttribute, messageId: " << request.messageId << ", key: " << request.key;

        const auto repo = Database::RepositoryFactory::instance().eqsRepository();
        std::optional<Database::Entity::EQS::Message> message = repo->findMessageByName(request.messageId);
        if (!message.has_value()) {
            return EqsServer::ErrorResponse(req, status::not_found, "Message not found, messageId: " + request.messageId);
        }

        // Set attribute and update in database
        message->attributes[request.key] = Dto::EQS::EqsMapper::toEntity(request.value);
        repo->upsertMessage(message.value());

        Dto::EQS::GetMessageAttributeResponse response;
        response.messageId = request.messageId;
        response.name = request.key;
        response.value = Dto::EQS::EqsMapper::toDto(message->attributes[request.key]);

        applyMetadata(response, *auth.user);
        return EqsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleGetMessageMetadata(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-message-metadata");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EQS::GetMessageMetadataRequest>(jv);
        log_info << "EQS GetMessageMetadata, messageId: " << request.messageId;

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
        response.created = Core::DateTimeUtils::ToISO8601(message->created);
        response.modified = Core::DateTimeUtils::ToISO8601(message->modified);
        applyMetadata(response, *auth.user);
        return EqsServer::JsonResponse(req, status::ok, response.toJson());
    }

    static response<string_body> handleGetQueueAttributes(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-metadata");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        log_info << "EQS GetQueueAttributes url=" << Core::GetStringValue(jv, "QueueUrl");

        const boost::json::object body{
                {"Attributes", boost::json::object{}},
                {"ResponseMetadata", boost::json::object{{"RequestId", EqsServer::RequestId()}}}
        };
        return EqsServer::JsonResponse(req, status::ok, boost::json::serialize(body));
    }

    static response<string_body> handleSetQueueAttributes(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "add-metadata");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        log_info << "EQS SetQueueAttributes url=" << Core::GetStringValue(jv, "QueueUrl");

        const boost::json::object body{
                {"ResponseMetadata", boost::json::object{{"RequestId", EqsServer::RequestId()}}}
        };
        return EqsServer::JsonResponse(req, status::ok, boost::json::serialize(body));
    }

    static response<string_body> handleAddQueueTag(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "add-queue-tag");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

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

    // Shared by stop-queue and start-queue, which differ only in the value they record.
    static response<string_body> handleSetQueueStopped(const request<string_body> &req, const bool stopped) {

        const auto action = stopped ? "stop-queue" : "start-queue";
        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", action);

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto ern = Core::GetStringValue(jv, "ern");
        if (ern.empty()) {
            return EqsServer::ErrorResponse(req, status::bad_request, "Queue ERN missing");
        }
        if (const auto denied = denyUngrantedQueue(req, auth, ern)) return *denied;

        const auto repo = Database::RepositoryFactory::instance().eqsRepository();
        std::optional<Database::Entity::EQS::Queue> queue = repo->findQueueByErn(ern);
        if (!queue.has_value()) {
            return EqsServer::ErrorResponse(req, status::not_found, "Queue not found, ern: " + ern);
        }

        log_info << "EQS " << action << ", ern: " << ern << ", available: " << queue->available;

        // Messages already in flight are left alone. Their consumer took them before the queue was
        // stopped and is still entitled to finish: deleting one is not a receive, and stopping a
        // queue should not turn every lease a consumer is holding into a redelivery.
        queue->status = stopped ? Database::Entity::EQS::QueueStatus::STOPPED : Database::Entity::EQS::QueueStatus::AVAILABLE;
        queue = repo->upsertQueue(queue.value());

        return EqsServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::object{
                                               {"ern", ern},
                                               {"status", Database::Entity::EQS::QueueStatusToString(queue->status)},
                                               {"available", queue->available}}));
    }

    static response<string_body> handleStopQueue(const request<string_body> &req) {
        return handleSetQueueStopped(req, true);
    }

    static response<string_body> handleStartQueue(const request<string_body> &req) {
        return handleSetQueueStopped(req, false);
    }

    static response<string_body> handleSetQueueVisibility(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "set-queue-visibility");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EQS::SetQueueVisibilityRequest>(jv);
        if (request.ern.empty()) {
            return EqsServer::ErrorResponse(req, status::bad_request, "Queue ERN missing");
        }
        // The same bounds handleSetVisibility() holds a single message to, and AWS SQS holds both
        // to: a queue default outside the range a message may be given would be a figure no
        // message could ever actually take.
        if (request.visibility < 0 || request.visibility > 43200) {
            return EqsServer::ErrorResponse(req, status::bad_request, "Visibility must be between 0 and 43200 seconds");
        }

        const auto repo = Database::RepositoryFactory::instance().eqsRepository();
        std::optional<Database::Entity::EQS::Queue> queue = repo->findQueueByErn(request.ern);
        if (!queue.has_value()) {
            return EqsServer::ErrorResponse(req, status::not_found, "Queue not found, ern: " + request.ern);
        }

        log_info << "EQS SetQueueVisibility, ern: " << request.ern << ", visibility: " << queue->visibility << " -> " << request.visibility;

        // Only the default changes. Messages already in flight keep the window they were given
        // when they were received - shortening it here would make a consumer's lease expire
        // under it while it is still working, and lengthening it would hold a message back that
        // its consumer has already given up on.
        queue->visibility = request.visibility;
        queue = repo->upsertQueue(queue.value());

        return EqsServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::object{
                                               {"ern", request.ern},
                                               {"visibility", queue->visibility}}));
    }

    static response<string_body> handleSetQueueDelay(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "set-queue-delay");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EQS::SetQueueDelayRequest>(jv);
        if (request.ern.empty()) {
            return EqsServer::ErrorResponse(req, status::bad_request, "Queue ERN missing");
        }
        // The bound AWS SQS holds DelaySeconds to. A delay is for smoothing a burst or letting a
        // writer finish, not for scheduling: something that has to wait a quarter of an hour wants
        // a timestamp of its own rather than a queue that holds everything back.
        if (request.delay < 0 || request.delay > 900) {
            return EqsServer::ErrorResponse(req, status::bad_request, "Delay must be between 0 and 900 seconds");
        }

        const auto repo = Database::RepositoryFactory::instance().eqsRepository();
        std::optional<Database::Entity::EQS::Queue> queue = repo->findQueueByErn(request.ern);
        if (!queue.has_value()) {
            return EqsServer::ErrorResponse(req, status::not_found, "Queue not found, ern: " + request.ern);
        }

        log_info << "EQS SetQueueDelay, ern: " << request.ern << ", delay: " << queue->delay << " -> " << request.delay;

        // Only what is sent from here on. A message already waiting had its delay turned into a
        // timestamp when it arrived, and moving that now would either release a message early or
        // hold back one that was promised sooner.
        queue->delay = request.delay;
        queue = repo->upsertQueue(queue.value());

        return EqsServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::object{
                                               {"ern", request.ern},
                                               {"delay", queue->delay}}));
    }

    static response<string_body> handleSetQueueMaxMessageLength(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "set-queue-max-message-length");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EQS::SetQueueMaxMessageLengthRequest>(jv);
        if (request.ern.empty()) {
            return EqsServer::ErrorResponse(req, status::bad_request, "Queue ERN missing");
        }
        // Zero is allowed and is not "accept nothing": it is the queue carrying no limit of its
        // own, which is what a queue created before the limit meant anything holds, and what
        // EffectiveMaxMessageLength() measures against the installation's figure instead.
        if (request.maxMessageLength < 0) {
            return EqsServer::ErrorResponse(req, status::bad_request,
                                            "maxMessageLength cannot be negative; zero follows the default of "
                                                    + std::to_string(Database::Entity::EQS::kDefaultMaxMessageLength) + " bytes");
        }

        const auto repo = Database::RepositoryFactory::instance().eqsRepository();
        std::optional<Database::Entity::EQS::Queue> queue = repo->findQueueByErn(request.ern);
        if (!queue.has_value()) {
            return EqsServer::ErrorResponse(req, status::not_found, "Queue not found, ern: " + request.ern);
        }

        log_info << "EQS SetQueueMaxMessageLength, ern: " << request.ern
                 << ", maxMessageLength: " << queue->maxMessageLength << " -> " << request.maxMessageLength;

        // What is sent from here on. A message already in the queue was measured against the limit
        // in force when it arrived, and lowering this is not a reason to go back and reject it.
        queue->maxMessageLength = request.maxMessageLength;
        queue = repo->upsertQueue(queue.value());

        return EqsServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::object{
                                               {"ern", request.ern},
                                               {"maxMessageLength", queue->maxMessageLength},
                                               // What a send is actually measured against, which is
                                               // not the stored figure when that is zero.
                                               {"effectiveMaxMessageLength",
                                                Database::Entity::EQS::EffectiveMaxMessageLength(queue->maxMessageLength)}}));
    }

    static response<string_body> handleSetQueueTag(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "set-queue-tag");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto [ern, key, value] = boost::json::value_to<Dto::EQS::AddQueueTagRequest>(jv);
        log_info << "EQS SetQueueTag, ern: " << ern << ", key: " << key;

        const auto repo = Database::RepositoryFactory::instance().eqsRepository();
        std::optional<Database::Entity::EQS::Queue> queue = repo->findQueueByErn(ern);
        if (!queue.has_value()) {
            return EqsServer::ErrorResponse(req, status::not_found, "Queue not found, ern: " + ern);
        }
        if (!queue.value().tags.contains(key)) {
            return EqsServer::ErrorResponse(req, status::not_found, "Tag not found, key: " + key);
        }
        queue->tags[key] = value;
        queue = repo->upsertQueue(queue.value());

        return EqsServer::JsonResponse(req, status::ok);
    }

    static response<string_body> handleDeleteQueueTag(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-queue-tag");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EqsServer::ParseJsonBody(req, jv)) return *err;

        const auto [ern, key] = boost::json::value_to<Dto::EQS::DeleteQueueTagRequest>(jv);
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
            GetQueue,
            GetMessage,
            GetQueueErn,
            GetMessageCount,
            GetQueueMetadata,
            GetMessageAttribute,
            SetMessageAttribute,
            GetMessageMetadata,
            ListQueues,
            ListMessages,
            SendMessage,
            SendMessageBatch,
            ReceiveMessages,
            SetVisibility,
            SetQueueVisibility,
            SetQueueDelay,
            SetQueueMaxMessageLength,
            StopQueue,
            StartQueue,
            DeleteMessage,
            PurgeQueue,
            PurgeAllQueues,
            RedriveDlq,
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
        if (action == "get-queue") return Command::GetQueue;
        if (action == "get-message") return Command::GetMessage;
        if (action == "get-queue-ern") return Command::GetQueueErn;
        if (action == "list-queues") return Command::ListQueues;
        if (action == "list-messages") return Command::ListMessages;
        if (action == "send-message") return Command::SendMessage;
        if (action == "send-message-batch") return Command::SendMessageBatch;
        if (action == "receive-messages") return Command::ReceiveMessages;
        // Two spellings, one command. "set-message-visibility" is the name that says what it
        // changes, and pairs with "set-queue-visibility"; "set-visibility" is what it was called
        // first and what euclid-jdk still sends, so it keeps working rather than breaking every
        // client built against it.
        if (action == "set-visibility" || action == "set-message-visibility") return Command::SetVisibility;
        if (action == "set-queue-visibility") return Command::SetQueueVisibility;
        if (action == "set-queue-delay") return Command::SetQueueDelay;
        if (action == "set-queue-max-message-length") return Command::SetQueueMaxMessageLength;
        if (action == "stop-queue") return Command::StopQueue;
        if (action == "start-queue") return Command::StartQueue;
        if (action == "delete-message") return Command::DeleteMessage;
        if (action == "purge-queue") return Command::PurgeQueue;
        if (action == "purge-all-queues") return Command::PurgeAllQueues;
        if (action == "redrive-dlq") return Command::RedriveDlq;
        if (action == "get-message-count") return Command::GetMessageCount;
        if (action == "get-queue-metadata") return Command::GetQueueMetadata;
        if (action == "get-message-attribute") return Command::GetMessageAttribute;
        if (action == "set-message-attribute") return Command::SetMessageAttribute;
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
            return EqsServer::ErrorResponse(req, status::bad_request, "Missing x-euclid-action header");
        }
        log_debug << "EQS action=" << action;

        switch (commandFromString(action)) {

            case Command::CreateQueue:
                return handleCreateQueue(req);

            case Command::DeleteQueue:
                return handleDeleteQueue(req);

            case Command::GetQueue:
                return handleGetQueue(req);

            case Command::GetMessage:
                return handleGetMessage(req);

            case Command::GetQueueErn:
                return handleGetQueueErn(req);

            case Command::ListQueues:
                return handleListQueues(req);

            case Command::ListMessages:
                return handleListMessages(req);

            case Command::SendMessage:
                return handleSendMessage(req);

            case Command::SendMessageBatch:
                return handleSendMessageBatch(req);

            case Command::ReceiveMessages:
                return handleReceiveMessage(req);

            case Command::SetVisibility:
                return handleSetVisibility(req);

            case Command::SetQueueVisibility:
                return handleSetQueueVisibility(req);

            case Command::SetQueueDelay:
                return handleSetQueueDelay(req);

            case Command::SetQueueMaxMessageLength:
                return handleSetQueueMaxMessageLength(req);

            case Command::StopQueue:
                return handleStopQueue(req);

            case Command::StartQueue:
                return handleStartQueue(req);

            case Command::DeleteMessage:
                return handleDeleteMessage(req);

            case Command::PurgeQueue:
                return handlePurgeQueue(req);

            case Command::PurgeAllQueues:
                return handlePurgeAllQueues(req);

            case Command::RedriveDlq:
                return handleRedriveDlq(req);

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

            case Command::SetMessageAttribute:
                return handleSetMessageAttribute(req);

            case Command::GetMessageMetadata:
                return handleGetMessageMetadata(req);

            case Command::AddQueueTag:
                return handleAddQueueTag(req);

            case Command::SetQueueTag:
                return handleSetQueueTag(req);

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

    // ── EventBus ─────────────────────────────────────────────────────────────
    // Consumer side of every EQS-type subscription in the system - ENS topics
    // (EnsServer::handlePublishMessage, event "ens.message.published") and ESM buckets
    // (EsmServer::notifyBucketSubscriptions, event "esm.subscription.delivery") both fan out through the same
    // payload shape (targetErn + body [+ attributes]), so one handler, registered for both event
    // types below, covers both: one delivery per subscribed queue, claimed by exactly one eqs
    // instance, turned into a real queue message here.

    static bool handleSubscriptionDelivery(const Database::EventEnvelope &envelope) {

        // The envelope first, the payload second. These moved from the payload onto the envelope
        // so the target could be indexed, and ees_events outlives any one deploy: events published
        // by the previous binary are still being consumed by this one for as long as it takes the
        // backlog to drain. Without the fallback every one of those looks like a delivery to
        // nowhere and is acked away - the messages are lost, and the log says the queue was not
        // found when the queue was fine all along.
        const auto targetErn = !envelope.targetErn.empty() ? envelope.targetErn : Core::GetStringValue(envelope.payload, "targetErn");
        const auto sourceMessageId = !envelope.messageId.empty() ? envelope.messageId : Core::GetStringValue(envelope.payload, "messageId");
        const auto body = Core::GetStringValue(envelope.payload, "body");

        const auto repo = Database::RepositoryFactory::instance().eqsRepository();
        const auto queue = repo->findQueueByErn(targetErn);
        if (!queue.has_value()) {

            // A missing queue is permanent, not transient. Acking this one delivery and moving on
            // would mean rediscovering the same fact for every event still queued for it - an
            // application that restarts a few times during a busy run leaves hundreds of thousands
            // of them, and each would produce a warning identical to this one.
            //
            // So the rest go with it. The queue is gone for all of them at once, which makes the
            // first delivery to notice the right place to clear the others.
            const auto discarded = Database::EventBus::instance().DiscardDeliveries(targetErn);

            // And the bucket subscription that keeps producing them, if it is still there: an
            // application that created a queue and a subscription and went away without removing
            // either leaves exactly this behind. Removed from here rather than by ESM because this
            // is where the queue's absence is discovered - ESM would have to read EQS's collection
            // on every single object to find it out for itself.
            const auto sourceErn = !envelope.sourceErn.empty() ? envelope.sourceErn : Core::GetStringValue(envelope.payload, "sourceErn");
            long removed = 0;
            for (const auto &subscription: Database::RepositoryFactory::instance().esmRepository()->listSubscriptionsBySourceErn(sourceErn)) {
                if (subscription.targetErn != targetErn) continue;
                Database::RepositoryFactory::instance().esmRepository()->deleteSubscriptionByErn(subscription.ern);
                ++removed;
            }

            log_warning << "EQS EventBus subscription delivery: target queue not found, ern: " << targetErn
                        << ", discarded undeliverable events: " << discarded
                        << ", removed bucket subscriptions: " << removed;
            return true;// ack - queue is gone, nothing to retry
        }

        const auto readAttributes = [&envelope](const char *field) {
            std::map<std::string, Database::Entity::COM::Variant> out;
            if (!envelope.payload.is_object()) return out;
            if (const auto *value = envelope.payload.as_object().if_contains(field); value && value->is_object()) {
                for (const auto &attribute: value->as_object()) {
                    // One attribute at a time, because one that cannot be read must not cost the
                    // message. The body is what the consumer is waiting for; throwing here fails
                    // the whole delivery, and after enough attempts the message goes to the dead
                    // letter queue over a piece of metadata nobody may even look at.
                    try {
                        out[attribute.key()] = Dto::EQS::EqsMapper::toEntity(boost::json::value_to<Dto::COM::Variant>(attribute.value()));
                    } catch (const std::exception &e) {
                        log_warning << "EQS subscription delivery: dropping unreadable " << field << " entry, name: "
                                    << attribute.key() << ", error: " << e.what();
                    }
                }
            }
            return out;
        };

        const auto attributes = readAttributes("attributes");

        // Euclid's own, kept apart from the caller's the whole way: they arrived on the object,
        // they travel with the delivery, and they go on to the message so the next hop still has
        // them. This is what a producer's decision rides on when the thing it has to survive - a
        // bucket - has no notion of it.
        const auto systemAttributes = readAttributes("systemAttributes");

        // Four sources, least specific first, each overriding the one before it. Written as four
        // assignments rather than a chain of else-ifs because the order *is* the rule, and a reader
        // should be able to see it without working out which branches exclude which.
        //
        // 1. The queue's own default is the floor, which is also what send-message starts from, so a
        //    message put into a queue by a subscription and one put there by a client are treated
        //    alike.
        //
        // 2. The source bucket's priority, if it has one: "everything that lands in this bucket is
        //    urgent". A bucket is not consumed from and has no use for a priority itself - it exists
        //    on the bucket only to be handed to the messages its notifications become, which is
        //    here. Sent by ESM as the bucket stored it, unresolved, because comparing it against the
        //    object's would mean ESM parsing a priority it has no use for.
        //
        // 3. The object's own, from its system attributes: "this one is". Narrower than the bucket's
        //    statement, so it wins - a bucket sets a floor without taking away the ability to say
        //    more about one object. Whatever wrote the object set it two hops upstream, and it
        //    travels the whole way so the producer's decision is not lost in the middle.
        //
        // 4. A priority carried on the delivery, which is what a topic hop puts there. Most specific
        //    because it is not a default at all: it is the priority a message already had.
        auto priority = queue->priority;
        if (const auto bucketDefault = Core::GetStringValue(envelope.payload, "bucketPriority"); !bucketDefault.empty()) {
            priority = Database::Entity::EQS::MessagePriorityFromString(bucketDefault);
        }
        if (const auto it = systemAttributes.find(Database::Entity::EQS::kPriorityAttribute);
            it != systemAttributes.end() && it->second.holds<std::string>()) {
            priority = Database::Entity::EQS::MessagePriorityFromString(it->second.get<std::string>());
        }
        if (const auto carried = Core::GetStringValue(envelope.payload, "priority"); !carried.empty()) {
            priority = Database::Entity::EQS::MessagePriorityFromString(carried);
        }

        const auto messageId = Core::UuidUtils::CreateRandomUuid();
        const auto ern = Core::createEqsMessageErn(Core::accountIdFromErn(targetErn), messageId);
        const auto message = repo->sendMessage(messageId, ern, targetErn, body, attributes, systemAttributes, priority);

        // A subscription delivery is a message sent to this queue like any other - counting it
        // only in the publishing module would leave the queue's own totals short of what it holds.
        recordMessagesSent(targetErn, 1, message.size);

        log_info << "EQS created message from subscription delivery, source: " << envelope.sourceModule << ", eventType: " << envelope.eventType
                  << ", targetErn: " << targetErn << ", messageId: " << messageId << ", sourceMessageId: " << sourceMessageId
                  << ", priority: " << Database::Entity::EQS::MessagePriorityToString(priority);
        return true;
    }

    // ── EqsServer ────────────────────────────────────────────────────────────

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
                if (auto &cfg = Core::Configuration::instance(); cfg.has("euclid.account-ids")) {
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

            resolve("queueErn", "eqs", "queue");

            // redrive-dlq's target. Named separately from "ern" because that one is already the
            // dead letter queue being emptied, and both are queue names a caller may type bare.
            resolve("targetErn", "eqs", "queue");

            // A bare "ern" too, unconditionally. It names a queue in most actions and an object,
            // message or subscription in the rest - but that distinction does not matter here,
            // because a full ERN is always passed through untouched and only a bare value is ever
            // resolved. A bare value in one of those other actions was never valid anyway, so the
            // worst this can do is fail with a different message than it used to.
            //
            // Deliberately not an action list: the wire field a DTO serialises to is not always
            // the name of its C++ member (PublishMessageRequest::topicErn is sent as "ern"), so
            // any list keyed on one would be wrong for the actions where the two disagree - which
            // is exactly how publish-message was missed.
            resolve("ern", "eqs", "queue");
        });
    }

    EqsServer::EqsServer(std::string socketPath, const int threads) : HttpActionServer("EQS", std::move(socketPath), threads) {
        installErnResolver();
        longPollSlots.limit(threads - 1);

        auto &scheduler = Core::Scheduler::instance();
        scheduler.Start();
        _resetMessagesTaskId = scheduler.SchedulePeriodic("queues-reset-expired-messages", [] {
                                                              Database::RepositoryFactory::instance().eqsRepository()->resetExpiredMessages();
                                                          },
                                                          std::chrono::seconds(30));

        Database::EventBus::instance().Subscribe("eqs", "ens.message.published", handleSubscriptionDelivery);
        Database::EventBus::instance().Subscribe("eqs", "esm.subscription.delivery", handleSubscriptionDelivery);
        Database::EventBus::instance().Start("eqs");
    }

    EqsServer::~EqsServer() {
        Core::Scheduler::instance().Cancel(_resetMessagesTaskId);
    }

    response<string_body> EqsServer::DispatchAction(const request<string_body> &req) {
        return dispatch(req);
    }

}// namespace Euclid::EQS