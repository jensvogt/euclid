// C++ includes
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Euclid includes
#include <EesServer.h>
#include <euclid/core/DateTimeUtils.h>
#include <euclid/core/monitoring/MonitoringTimer.h>
#include <euclid/database/entity/eam/User.h>

namespace Euclid::EES {

    namespace {
        constexpr auto kServiceTimer = "ees-service-time";
        constexpr auto kServiceCounter = "ees-service-count";

        // How long a received event stays invisible to other claimers before it comes back.
        // Generous compared with the bus's own 30 seconds for modules: a consumer here is an
        // application that may be doing real work with the event - downloading the object it
        // describes, say - and having it redelivered underneath itself is worse than waiting.
        constexpr long kDefaultVisibilitySeconds = 300;

        // Longest a receive call waits for an event before answering with none. Bounded because
        // the caller is holding a gateway worker thread for the duration.
        constexpr long kMaxWaitSeconds = 20;

        // How many of this process's worker threads may be sitting in that wait at once. Set from
        // the thread count in the constructor, always leaving one thread over: without it, enough
        // clients waiting for events leave nothing to answer a subscribe, and the subscribe does
        // not fail - it queues, and is served the moment a wait ends, which is often just after
        // the client that sent it has given up. See Core::LongPollSlots.
        Core::LongPollSlots longPollSlots;

        struct AuthResult {
            std::optional<Database::Entity::EAM::User> user;
            bool tokenExpired{false};
            std::string denialReason;
        };

        std::string stringField(const boost::json::object &obj, const std::string &key, const std::string &fallback = "") {
            if (const auto *v = obj.if_contains(key); v && v->is_string()) return v->as_string().c_str();
            return fallback;
        }

        long longField(const boost::json::object &obj, const std::string &key, const long fallback = 0) {
            if (const auto *v = obj.if_contains(key); v && v->is_number()) return v->to_number<long>();
            return fallback;
        }

        std::vector<std::string> stringArray(const boost::json::object &obj, const std::string &key) {
            std::vector<std::string> result;
            if (const auto *v = obj.if_contains(key); v && v->is_array()) {
                for (const auto &element: v->as_array()) {
                    if (element.is_string()) result.emplace_back(element.as_string().c_str());
                }
            }
            return result;
        }

        boost::json::object objectField(const boost::json::object &obj, const std::string &key) {
            if (const auto *v = obj.if_contains(key); v && v->is_object()) return v->as_object();
            return {};
        }

        boost::json::object toJson(const Database::EventBus::Subscription &subscription) {
            return boost::json::object{
                    {"subscriber", subscription.subscriber},
                    {"eventType", subscription.eventType},
                    {"filter", subscription.filter},
                    {"accountId", subscription.accountId},
                    {"mode", std::string(Database::EventBus::DeliveryModeToString(subscription.mode))},
                    {"created", Core::DateTimeUtils::ToISO8601(subscription.createdAt)},
                    {"lastSeen", Core::DateTimeUtils::ToISO8601(subscription.lastSeenAt)}};
        }

        boost::json::object toJson(const Database::EventEnvelope &envelope) {
            return boost::json::object{
                    {"eventId", envelope.eventId},
                    {"eventType", envelope.eventType},
                    {"sourceModule", envelope.sourceModule},
                    {"payload", envelope.payload},
                    {"attempts", envelope.attempts},
                    {"created", Core::DateTimeUtils::ToISO8601(envelope.createdAt)}};
        }

    }// namespace

    static AuthResult authenticate(const request<string_body> &req) {
        const auto auth = EesServer::Authenticate(req);
        if (!auth.subject.has_value()) {
            return {.user = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason};
        }
        return {.user = Database::RepositoryFactory::instance().eamRepository()->findUserByUserId(*auth.subject)};
    }

    static response<string_body> unauthorized(const request<string_body> &req, const AuthResult &auth) {
        return EesServer::Unauthorized(req, {.subject = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason});
    }

    // ── Action handlers ──────────────────────────────────────────────────────

    static response<string_body> handleSubscribeEvents(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "subscribe-events");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EesServer::ParseJsonBody(req, jv)) return *err;
        if (!jv.is_object()) return EesServer::ErrorResponse(req, status::bad_request, "Expected a JSON object body");
        const auto &obj = jv.as_object();

        const auto subscriber = stringField(obj, "name");
        if (subscriber.empty()) return EesServer::ErrorResponse(req, status::bad_request, "name is required");

        const auto eventTypes = stringArray(obj, "eventTypes");
        if (eventTypes.empty()) return EesServer::ErrorResponse(req, status::bad_request, "eventTypes is required");

        // The filter is what keeps a subscriber's backlog proportional to what it asked for
        // rather than to how busy the installation is, so it is worth saying it is optional
        // rather than silently accepting nothing: an empty filter really does mean every event of
        // these types.
        const auto filter = objectField(obj, "filter");

        // Durable unless the caller says otherwise: a subscriber that asked for events and was
        // not connected when one arrived should find it waiting, and anything else has to be
        // chosen deliberately.
        const auto modeField = stringField(obj, "mode", "durable");
        if (modeField != "durable" && modeField != "live") {
            return EesServer::ErrorResponse(req, status::bad_request, R"(mode must be "durable" or "live")");
        }
        const auto mode = Database::EventBus::DeliveryModeFromString(modeField);

        // Set by the gateway for a subscription that belongs to one websocket connection: it is
        // removed when that connection ends, and any that survive a crash are cleared when the
        // gateway next starts. Nothing stops a client setting it, and nothing goes wrong if one
        // does - the subscription simply does not outlive this gateway process.
        const auto *ephemeralValue = obj.if_contains("ephemeral");
        const auto ephemeral = ephemeralValue != nullptr && ephemeralValue->is_bool() && ephemeralValue->as_bool();

        auto &bus = Database::EventBus::instance();
        for (const auto &eventType: eventTypes) {
            bus.SubscribeExternal(subscriber, eventType, filter, auth.user->accountId, mode, ephemeral);
        }

        boost::json::array subscriptions;
        for (const auto &subscription: bus.ListSubscriptions(subscriber)) subscriptions.push_back(toJson(subscription));

        log_info << "EES subscribed, subscriber: " << subscriber << ", eventTypes: " << eventTypes.size() << ", mode: " << modeField;

        return EesServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::object{{"subscriptions", subscriptions}}));
    }

    static response<string_body> handleUnsubscribeEvents(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "unsubscribe-events");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EesServer::ParseJsonBody(req, jv)) return *err;
        if (!jv.is_object()) return EesServer::ErrorResponse(req, status::bad_request, "Expected a JSON object body");
        const auto &obj = jv.as_object();

        const auto subscriber = stringField(obj, "name");
        if (subscriber.empty()) return EesServer::ErrorResponse(req, status::bad_request, "name is required");

        // Naming no event type removes the subscriber entirely, which is what an application
        // being decommissioned wants; naming one narrows it.
        const auto removed = Database::EventBus::instance().UnsubscribeExternal(subscriber, stringField(obj, "eventType"));
        log_info << "EES unsubscribed, subscriber: " << subscriber << ", removed: " << removed;

        return EesServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::object{{"subscriber", subscriber}, {"removed", removed}}));
    }

    static response<string_body> handleListSubscriptions(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-subscriptions");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EesServer::ParseJsonBody(req, jv)) return *err;
        if (!jv.is_object()) return EesServer::ErrorResponse(req, status::bad_request, "Expected a JSON object body");

        const auto subscriber = stringField(jv.as_object(), "name");
        if (subscriber.empty()) return EesServer::ErrorResponse(req, status::bad_request, "name is required");

        auto &bus = Database::EventBus::instance();

        boost::json::array subscriptions;
        for (const auto &subscription: bus.ListSubscriptions(subscriber)) subscriptions.push_back(toJson(subscription));

        return EesServer::JsonResponse(req, status::ok,
                                       boost::json::serialize(boost::json::object{
                                               {"subscriptions", subscriptions},
                                               {"waiting", bus.CountEvents(subscriber)}}));
    }

    static response<string_body> handleReceiveEvents(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "receive-events");

        if (const auto auth = authenticate(req); !auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EesServer::ParseJsonBody(req, jv)) return *err;
        if (!jv.is_object()) return EesServer::ErrorResponse(req, status::bad_request, "Expected a JSON object body");
        const auto &obj = jv.as_object();

        const auto subscriber = stringField(obj, "name");
        if (subscriber.empty()) return EesServer::ErrorResponse(req, status::bad_request, "name is required");

        const auto maxEvents = std::max(1L, longField(obj, "maxEvents", 10));
        const auto waitTime = std::clamp(longField(obj, "waitTime", 0), 0L, kMaxWaitSeconds);
        const auto visibility = std::chrono::seconds(std::max(1L, longField(obj, "visibilityTimeout", kDefaultVisibilitySeconds)));

        auto &bus = Database::EventBus::instance();

        // Long-polled the way a queue receive is: an empty answer that took twenty seconds beats
        // a client asking again every hundred milliseconds. The wait is bounded because a gateway
        // worker thread is held for its duration - and capped in number for the same reason, so
        // that clients waiting for events can never take the last thread away from a client with
        // something to do. Without a slot this answers with whatever is there right now, which is
        // what a long poll returns anyway when nothing arrives; the client asks again.
        const auto slot = longPollSlots.acquire();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(slot.held() ? waitTime : 0);
        std::vector<Database::EventEnvelope> events;
        while (true) {
            events = bus.ClaimEvents(subscriber, maxEvents, visibility);
            if (!events.empty() || std::chrono::steady_clock::now() >= deadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }

        boost::json::array payload;
        for (const auto &event: events) payload.push_back(toJson(event));

        return EesServer::JsonResponse(req, status::ok,
                                       boost::json::serialize(boost::json::object{{"events", payload}, {"total", static_cast<long>(events.size())}}));
    }

    static response<string_body> handleAckEvents(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "ack-events");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EesServer::ParseJsonBody(req, jv)) return *err;
        if (!jv.is_object()) return EesServer::ErrorResponse(req, status::bad_request, "Expected a JSON object body");
        const auto &obj = jv.as_object();

        const auto subscriber = stringField(obj, "name");
        if (subscriber.empty()) return EesServer::ErrorResponse(req, status::bad_request, "name is required");

        const auto eventIds = stringArray(obj, "eventIds");
        if (eventIds.empty()) return EesServer::ErrorResponse(req, status::bad_request, "eventIds is required");

        auto &bus = Database::EventBus::instance();

        // An event that is not there any more is not an error: a redelivery acknowledged twice,
        // or one whose retention ran out, both end here and both mean the same thing to a caller.
        long acknowledged = 0;
        for (const auto &eventId: eventIds) {
            if (bus.AckEvent(subscriber, eventId)) ++acknowledged;
        }

        log_debug << "EES acknowledged, subscriber: " << subscriber << ", count: " << acknowledged;

        return EesServer::JsonResponse(req, status::ok,
                                       boost::json::serialize(boost::json::object{
                                               {"subscriber", subscriber},
                                               {"acknowledged", acknowledged},
                                               {"waiting", bus.CountEvents(subscriber)}}));
    }

    // ── Request dispatcher ───────────────────────────────────────────────────

    static response<string_body> dispatch(const request<string_body> &req) {

        const auto action = std::string(req["x-euclid-action"]);
        if (action.empty()) {
            return EesServer::ErrorResponse(req, status::bad_request, "Missing x-euclid-action header");
        }
        log_debug << "EES action=" << action;

        if (action == "subscribe-events") return handleSubscribeEvents(req);
        if (action == "unsubscribe-events") return handleUnsubscribeEvents(req);
        if (action == "list-subscriptions") return handleListSubscriptions(req);
        if (action == "receive-events") return handleReceiveEvents(req);
        if (action == "ack-events") return handleAckEvents(req);
        if (action == "get-metrics") return EesServer::MetricsResponse(req);

        return EesServer::ErrorResponse(req, status::not_found, "Action not implemented: " + action);
    }

    // ── EesServer ────────────────────────────────────────────────────────────

    EesServer::EesServer(std::string socketPath, const int threads) : HttpActionServer("EES", std::move(socketPath), threads) {
        longPollSlots.limit(threads - 1);
    }

    response<string_body> EesServer::Dispatch(const request<string_body> &req) {
        return dispatch(req);
    }

}// namespace Euclid::EES