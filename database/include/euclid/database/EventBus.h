#pragma once

// C++ includes
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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
     * @par Delivery trigger
     * A background thread watches "internal_events" via a MongoDB change stream (requires the
     * server to run as a replica set - even a single-node one - which is what enables change
     * streams at all), filtered server-side to inserts targeting this moduleType, and attempts a
     * claim the moment one arrives - this is what gives near-instant delivery instead of waiting
     * out a poll interval. The stream is only a wake-up signal though; the actual claim still
     * goes through the same atomic find-and-update either way, so correctness never depends on
     * the stream not missing anything. Start()'s pollInterval is a much slower safety-net sweep
     * layered on top, catching anything a dead/reconnecting stream would otherwise leave stranded
     * (e.g. the gap before the watch thread's first connection succeeds); it also intentionally
     * doesn't fire on updates (like reap()'s status reset), so a reaped delivery is picked up by
     * this sweep or the next stream event for a fresh insert, not necessarily instantly.
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
         * @brief Starts this process's change-stream watcher (the primary, low-latency delivery
         * path), a safety-net poll at pollInterval, and stuck-claim reaping. Call once, after all
         * Subscribe() calls for this process.
         *
         * @param pollInterval cadence of the safety-net sweep, not the normal delivery latency -
         * see the class-level "Delivery trigger" note. Defaults to a slow 30s since the change
         * stream handles the normal case.
         */
        void Start(const std::string &moduleType, std::chrono::milliseconds pollInterval = std::chrono::seconds(30));

        /**
         * @brief Publishes an event, fanning it out to every module type currently subscribed to
         * eventType (one delivery per subscribing module type, regardless of how many instances
         * of that module type are running). A no-op if nobody is subscribed. Requires
         * Database::instance().initialize() to already have run.
         */
        void Publish(const std::string &eventType, const boost::json::value &payload, const std::string &sourceModule);

        // ── External subscribers ─────────────────────────────────────────────
        //
        // The same bus, for consumers that are not euclid modules: an application, an SDK, a
        // Spring service. They differ from a module type in three ways and in nothing else.
        //
        // They register a durable *name* rather than a module type, and get their own envelope
        // per event exactly as a module type does - so two applications watching the same bucket
        // both receive it, while two instances of one application share a name and whichever
        // claims it first processes it. That is what makes a queue per consumer unnecessary.
        //
        // They pull rather than being called: no handler exists in any process, so an envelope
        // waits until ClaimEvents() takes it and AckEvent() deletes it. Nothing else removes it,
        // and an unacked claim becomes visible again when its lease runs out.
        //
        // And they carry a filter, evaluated at publish time, so a subscriber only ever stores
        // the events it asked for rather than everything of that type in the installation.

        /**
         * @brief How a subscriber's events reach it.
         *
         * @par
         * DURABLE stores an envelope per event and keeps it until the subscriber acknowledges
         * it, so nothing is lost while the subscriber is away. When a websocket session is
         * attached to the name, a notification is pushed as well - not the event itself, because
         * two instances sharing a name must not both act on it, and the atomic claim in
         * ClaimEvents() is what decides which one does.
         *
         * @par
         * LIVE stores nothing: the event is pushed to whatever sessions are attached and is gone.
         * That is what a view wants - a UI showing a bucket has no use for the hour of events it
         * missed while nobody was looking at it, and paying a database write per event for them
         * is worse than not having them.
         */
        enum class DeliveryMode { Durable, Live };

        /**
         * @brief One external subscription, as returned by ListSubscriptions().
         */
        struct Subscription {
            std::string subscriber;
            std::string eventType;
            boost::json::object filter;
            std::string accountId;
            DeliveryMode mode = DeliveryMode::Durable;
            system_clock::time_point createdAt;
            system_clock::time_point lastSeenAt;
        };

        /**
         * @brief Registers a durable external subscription, or updates its filter.
         *
         * @param subscriber name the consumer claims events under; instances of one application
         * share it deliberately, so an event is processed once between them.
         * @param eventType event type to receive, e.g. "esm.object.created".
         * @param filter exact-match key/value pairs the event payload must satisfy; empty
         * receives every event of this type.
         * @param accountId account the subscriber belongs to - an event whose payload names a
         * different account is never stored for it.
         * @param mode whether events are kept until acknowledged or only pushed to whoever is
         * connected - see DeliveryMode.
         */
        void SubscribeExternal(const std::string &subscriber, const std::string &eventType,
                               const boost::json::object &filter, const std::string &accountId,
                               DeliveryMode mode = DeliveryMode::Durable, bool ephemeral = false);

        /**
         * @brief Removes every ephemeral subscription, i.e. every one that belonged to a
         * websocket connection rather than to a client that named itself.
         *
         * @par
         * Called by the manager as the gateway starts. An ephemeral subscription exists only for
         * as long as the connection that created it, and no connection can outlive the gateway
         * process - so any left in the database are from a previous run that ended without
         * cleaning up, and none can belong to a live session. They store nothing (ephemeral
         * subscriptions are live-mode), so this is tidiness rather than a leak, but a listing
         * full of dead names is its own kind of cost.
         *
         * @return number of subscriptions removed.
         */
        long PurgeEphemeralSubscriptions();

        /**
         * @brief The string a DeliveryMode is stored and reported as ("durable"/"live").
         */
        [[nodiscard]]
        static std::string_view DeliveryModeToString(DeliveryMode mode);

        /**
         * @brief Reads a DeliveryMode back, defaulting to durable for anything unrecognized -
         * losing events because a mode was misspelled would be the worse failure.
         */
        [[nodiscard]]
        static DeliveryMode DeliveryModeFromString(std::string_view mode);

        /**
         * @brief Removes an external subscription, and every event still waiting for it.
         *
         * @param subscriber subscriber name.
         * @param eventType event type to stop receiving; empty removes all of this subscriber's.
         * @return number of subscriptions removed.
         */
        long UnsubscribeExternal(const std::string &subscriber, const std::string &eventType);

        /**
         * @brief Lists a subscriber's subscriptions.
         *
         * @param subscriber subscriber name.
         * @return its subscriptions.
         */
        [[nodiscard]]
        std::vector<Subscription> ListSubscriptions(const std::string &subscriber) const;

        /**
         * @brief Claims up to maxEvents of a subscriber's waiting events.
         *
         * @par
         * The same atomic claim a module instance makes: an event handed out here is invisible to
         * any other claimer until the lease expires, and comes back if it is never acked - so a
         * consumer that crashes mid-work loses nothing.
         *
         * @param subscriber subscriber name.
         * @param maxEvents largest number of events to claim.
         * @param visibility how long the claim holds before the events become claimable again.
         * @return the claimed events, oldest first.
         */
        [[nodiscard]]
        std::vector<EventEnvelope> ClaimEvents(const std::string &subscriber, long maxEvents, std::chrono::seconds visibility);

        /**
         * @brief Deletes a claimed event, which is what "processed" means here.
         *
         * @param subscriber subscriber name the event was claimed under.
         * @param eventId ID from the claimed envelope.
         * @return true if an event was deleted.
         */
        bool AckEvent(const std::string &subscriber, const std::string &eventId);

        /**
         * @brief Number of events waiting for a subscriber, claimed or not.
         *
         * @param subscriber subscriber name.
         * @return the count.
         */
        [[nodiscard]]
        long CountEvents(const std::string &subscriber) const;

        /**
         * @brief The prefix an external subscriber's name carries internally - see kExternalPrefix.
         */
        [[nodiscard]]
        static constexpr std::string_view kExternalPrefixValue() { return "client:"; }

    private:

        EventBus() = default;

        void ensureIndexes();

        void pruneStaleSubscriptions(const std::string &moduleType);

        /**
         * @brief One subscription as Publish() needs it: the filter already parsed, the mode
         * already decoded, no BSON left.
         */
        struct SubscriberRecord {
            std::string target;// moduleType, or "client:<name>" for an external subscriber
            bool external = false;
            boost::json::object filter;
            std::string accountId;
            DeliveryMode mode = DeliveryMode::Durable;
        };

        /**
         * @brief The subscribers of one event type, as of shortly ago.
         *
         * @par
         * Disabled by default (euclid.eventbus.subscription-cache-seconds = 0), because it was
         * not worth its cost when measured: publishing queries the subscription collection every
         * time, and at ~800 messages/second against a local database that query did not show up
         * next to the send itself. Enabling it trades immediacy for fewer queries - a change made
         * in this process still takes effect at once (the entry is dropped), but one made in
         * another process takes up to the configured lifetime to be seen, which for a client that
         * subscribes and immediately triggers an event means missing it.
         * @par
         * Handed out as a shared_ptr to an immutable list rather than as a reference into the
         * cache: another thread refreshing the same event type would otherwise replace the
         * vector a publisher is still walking.
         */
        [[nodiscard]]
        std::shared_ptr<const std::vector<SubscriberRecord>> subscribersOf(const std::string &eventType);

        void invalidateSubscribers(const std::string &eventType);

        void pushToSessions(const std::string &eventType, const boost::json::object &payload,
                            const std::set<std::string> &durableTargets, const std::set<std::string> &liveTargets);

        /**
         * @brief One pending "you have events" notice, and what it needs to be delivered.
         */
        struct PendingNotify {
            std::string subscriber;
            std::string eventType;
            std::string accountId;
            std::string region;
        };

        /**
         * @brief Sends a durable subscriber's notice now, or remembers it for the next flush.
         *
         * @par
         * A notice carries no payload - it says only that something is waiting - so collapsing a
         * thousand of them into one loses nothing at all, and a purge of a bucket with a durable
         * subscriber attached is exactly that thousand. The first is sent immediately, because a
         * client should hear about a single event without waiting for a timer; further ones for
         * the same subscriber and event type are held until the flush interval passes, which is
         * what turns a burst into one notice rather than one per event.
         */
        void notifySubscriber(const PendingNotify &pending);

        void flushNotifications();

        void pollOnce(const std::string &moduleType);

        void watchLoop(const std::string &moduleType);

        static void reap();

        static void moveToDlq(const bsoncxx::document::view &doc, const std::string &reason);

        static constexpr auto SUBSCRIPTION_COLLECTION = "ees_subscriptions";
        static constexpr auto EVENT_COLLECTION = "ees_events";
        static constexpr auto DLQ_COLLECTION = "ees_events_dlq";
        /**
         * @brief Prefix an external subscriber's name carries in an event's targetModule.
         *
         * External subscribers share the module fan-out's key space rather than getting one of
         * their own: a subscription is still (targetModule, eventType), and no module can be
         * called "client:..." - which keeps publish, claim and reap one code path instead of two.
         */
        static constexpr auto kExternalPrefix = "client:";

        /**
         * @brief How long an unclaimed external event is kept before MongoDB expires it.
         *
         * A consumer that never comes back must not accumulate events forever. Module deliveries
         * carry no expiry field at all and are unaffected by the TTL index.
         */
        static constexpr auto kExternalRetentionSeconds = 7 * 24 * 3600;

        static constexpr int kMaxAttempts = 5;
        static constexpr int kBatchSize = 10;
        static constexpr auto kVisibilityTimeout = std::chrono::seconds(30);
        static constexpr auto kWatchReconnectDelay = std::chrono::seconds(2);

        std::once_flag _indexesOnce;
        std::string _instanceId;

        std::mutex _cacheMutex;

        struct CacheEntry {
            std::chrono::steady_clock::time_point refreshedAt;
            std::shared_ptr<const std::vector<SubscriberRecord>> subscribers;
        };

        std::unordered_map<std::string, CacheEntry> _subscriberCache;

        std::mutex _notifyMutex;
        std::map<std::string, PendingNotify> _pendingNotifications;// keyed by subscriber + event type
        std::map<std::string, std::chrono::steady_clock::time_point> _lastNotifiedAt;
        std::once_flag _notifyFlushOnce;

        std::mutex _handlersMutex;
        std::unordered_map<std::string, std::function<bool(const EventEnvelope &)> > _handlers;// keyed by eventType
    };

}// namespace Euclid::Database