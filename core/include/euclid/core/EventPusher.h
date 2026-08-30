#pragma once

// C++ includes
#include <string>

// Boost includes
#include <boost/json/object.hpp>

namespace Euclid::Core {

    /**
     * @brief Pushes a single business event from a module process to the gateway's websocket
     * clients, e.g. "a key was created" - the send side of the gateway's
     * GatewayWsRegistry::Broadcast(), which fans it out to every connected websocket client
     * (e.g. Euclid-JDK) scoped to the event's account/region.
     *
     * Modeled on Core::Monitoring::MetricsPusher's one-shot outbound Unix-domain-socket push
     * (module -> monitoring module), but event-driven rather than scheduled: a module calls
     * Push() inline, right when something worth telling connected clients about happens, instead
     * of on a timer. Unlike MetricsPusher, this has no owned lifecycle/instance - every call is
     * self-contained and fire-and-forget: on failure (gateway not running, socket path not
     * configured, etc.) it logs a warning and returns rather than blocking or retrying, so a
     * module's request-handling thread is never stalled waiting on the gateway.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class EventPusher {
    public:

        /**
         * @brief Registers the gateway's event-ingest Unix domain socket path, read once at
         * process startup from euclid.gateway.event-socket-path. Until this is called (or if the
         * configured path is empty), Push() is a no-op.
         *
         * @param path Unix domain socket path the gateway's GatewayEventIngest listens on.
         */
        static void SetGatewaySocketPath(std::string path);

        /**
         * @brief Pushes one event to every websocket client currently connected to the gateway
         * and scoped to accountId/region.
         *
         * @param topic     event topic, e.g. "ekm.key.created" - namespaced by module for clarity.
         * @param accountId account the event belongs to; only websocket sessions authenticated
         * for this account receive it.
         * @param region    region the event belongs to, same scoping role as accountId.
         * @param body      event payload.
         */
        static void Push(const std::string &topic, const std::string &accountId, const std::string &region, const boost::json::object &body);
    };

}// namespace Euclid::Core
