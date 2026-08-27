#pragma once

// C++ includes
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

// Boost includes
#include <boost/json.hpp>

// Mongodb includes
#include <bsoncxx/document/view.hpp>

namespace Euclid::Database {

    using std::chrono::system_clock;

    /**
     * @brief One delivery of a published event to a subscribing module type.
     */
    struct EventEnvelope {

        /**
         * @brief ID of this delivery (not the same value across different target modules for the
         * same Publish() call - each fan-out target gets its own envelope/ID).
         */
        std::string eventId;

        /**
         * @brief Producer-namespaced event type, e.g. "esm.bucket.deleted".
         */
        std::string eventType;

        /**
         * @brief Module type that published the event, e.g. "esm".
         */
        std::string sourceModule;

        /**
         * @brief Arbitrary event payload.
         */
        boost::json::value payload;

        /**
         * @brief Number of times this delivery has been claimed (including the current claim).
         */
        long attempts = 0;

        /**
         * @brief When the event was published.
         */
        system_clock::time_point createdAt;
    };

    /**
     * @brief Internal, cross-process event bus for module-to-module notifications.
     *
     * @par
     * Every euclid module runs as its own OS process, and a module type may run several
     * instances at once (see RepositoryFactory/MongoEqsRepository's queue-claim pattern, which
     * this mirrors). EventBus lets module A publish a domain event (e.g. "esm.bucket.deleted")
     * that fans out to every subscribing module *type* exactly once each, while - within a
     * module type - only one of its running instances ever processes a given delivery. There is
     * no user-visible resource here: no queue or topic is exposed via CLI/DTO/HTTP, unlike
     * EQS/ENS. Two collections back this, both private to EventBus: "internal_event_subscriptions"
     * (who wants what) and "internal_events" (pending/claimed deliveries); failed deliveries that
     * exhaust their retry budget are moved to "internal_events_dlq".
     *
     * @par Delivery semantics
     * At-least-once, no ordering guarantee (same as an SQS standard queue). A delivery is claimed
     * via an atomic find-and-update (status PENDING -> CLAIMED, stamped with this process's
     * instance ID and a visibility deadline); if the handler doesn't ack in time - including on
     * a crash - a periodic reap resets it to PENDING for another instance to pick up. After
     * kMaxAttempts failed/timed-out attempts, a delivery is moved to the DLQ collection instead
     * of retried forever.
     *
     * @par Usage
     * @code
     * // In modules/esm/src/main.cpp, after Database::instance().initialize():
     * EventBus::instance().Subscribe("eqs", "esm.bucket.deleted", [](const EventEnvelope &e) {
     *     // handle e.payload; return true to ack, false to retry
     *     return true;
     * });
     * EventBus::instance().Start("eqs");
     *
     * // Anywhere, including from a different module's process:
     * EventBus::instance().Publish("esm.bucket.deleted", boost::json::value{{"ern", ern}}, "esm");
     * @endcode
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class EventBus {

    public:

        /**
         * @brief Singleton instance
         */
        static EventBus &instance() {
            static EventBus bus;
            return bus;
        }

        EventBus(const EventBus &) = delete;
        EventBus &operator=(const EventBus &) = delete;

        /**
         * @brief Registers this process's interest in an event type.
         *
         * Idempotent and persisted (upserted into "internal_event_subscriptions"), so a
         * publisher never needs to know its subscribers - and future modules can subscribe
         * without any change to the publishing module. Call before Start(); requires
         * Database::instance().initialize() to already have run.
         *
         * @param moduleType this process's module type, e.g. "eqs"
         * @param eventType event type to subscribe to, e.g. "esm.bucket.deleted"
         * @param handler invoked once per claimed delivery; return true to ack (delete the
         * delivery), false to retry it. Exceptions are caught and treated as a failed attempt.
         */
        void Subscribe(const std::string &moduleType, const std::string &eventType,
                        std::function<bool(const EventEnvelope &)> handler);

        /**
         * @brief Starts this process's poll loop for moduleType's claimed deliveries and stuck-claim
         * reaping. Call once, after all Subscribe() calls for this process.
         */
        void Start(const std::string &moduleType, std::chrono::milliseconds pollInterval = std::chrono::seconds(2));

        /**
         * @brief Publishes an event, fanning it out to every module type currently subscribed to
         * eventType (one delivery per subscribing module type, regardless of how many instances
         * of that module type are running). A no-op if nobody is subscribed. Requires
         * Database::instance().initialize() to already have run.
         */
        void Publish(const std::string &eventType, const boost::json::value &payload, const std::string &sourceModule);

    private:

        EventBus() = default;

        void ensureIndexes();
        void pollOnce(const std::string &moduleType);
        void reap();
        void moveToDlq(const bsoncxx::document::view &doc, const std::string &reason);

        static constexpr auto SUBSCRIPTION_COLLECTION = "internal_event_subscriptions";
        static constexpr auto EVENT_COLLECTION = "internal_events";
        static constexpr auto DLQ_COLLECTION = "internal_events_dlq";
        static constexpr int kMaxAttempts = 5;
        static constexpr int kBatchSize = 10;
        static constexpr auto kVisibilityTimeout = std::chrono::seconds(30);

        std::once_flag _indexesOnce;
        std::string _instanceId;

        std::mutex _handlersMutex;
        std::unordered_map<std::string, std::function<bool(const EventEnvelope &)>> _handlers;// keyed by eventType
    };

}// namespace Euclid::Database
