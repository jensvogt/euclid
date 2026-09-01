#pragma once

// C++ includes
#include <string>

// Boost includes
#include <boost/json/object.hpp>

namespace Euclid::Core {

    /**
     * @brief Pushes a single event from a module process to the websocket sessions attached to
     * one EES subscriber - the send side of the gateway's
     * GatewayWsRegistry::DeliverToSubscriber().
     *
     * @par
     * Called by Database::EventBus when it publishes an event that a subscriber matched, never
     * by a module directly: what a client receives is decided by its subscription, and a module
     * that pushed events at connections would be a second way to decide that.
     *
     * Modeled on Core::Monitoring::MetricsPusher's one-shot outbound Unix-domain-socket push
     * (module -> monitoring module), but event-driven rather than scheduled: it happens inline,
     * right when the event is published, instead of on a timer. Unlike MetricsPusher, this has no
     * owned lifecycle/instance - every call is self-contained and fire-and-forget: on failure
     * (gateway not running, socket path not configured, etc.) it logs a warning and returns
     * rather than blocking or retrying, so a module's request-handling thread is never stalled
     * waiting on the gateway. A push that fails costs latency and nothing else: a durable
     * subscriber still has the event waiting for it.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class EventPusher {
    public:

        /**
         * @brief Registers the gateway's event-ingest Unix domain socket path, read once at
         * process startup from euclid.gateway.event-socket-path. Optional: the path is read from
         * the configuration on first use when it was never set, and pushing is a no-op while it
         * is empty.
         *
         * @param path Unix domain socket path the gateway's GatewayEventIngest listens on.
         */
        static void SetGatewaySocketPath(std::string path);

        /**
         * @brief Pushes one event to the websocket sessions attached to a named EES subscriber,
         * rather than to every session that happens to have asked for this topic.
         *
         * @par
         * This is the live end of a durable subscription: which events belong to the name was
         * already decided when the event was published (its filter, its account), so the gateway
         * only has to find the sessions. A subscriber with no session connected loses nothing -
         * a durable subscription still has the event waiting, and this push is only what saves
         * the client from asking.
         *
         * @param subscriber EES subscriber name.
         * @param topic      event topic, e.g. "esm.object.created".
         * @param accountId  account the event belongs to.
         * @param region     region the event belongs to.
         * @param body       event payload.
         */
        static void PushToSubscriber(const std::string &subscriber, const std::string &topic, const std::string &accountId,
                                     const std::string &region, const boost::json::object &body);
    };

}// namespace Euclid::Core
