#pragma once

// C++ includes
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Boost includes
#include <boost/json/object.hpp>

// Euclid includes
#include <euclid/manager/GatewayWsSession.h>

namespace Euclid::main {

    /**
     * @brief In-process registry of every currently-connected websocket session (see
     * GatewayWsSession/GatewayWsTlsSession), keyed by the accountId/region it authenticated for
     * at handshake time - lets GatewayEventIngest's Dispatch() find the sessions an event
     * pushed by Database::EventBus should reach.
     *
     * The first working version of the fan-out-to-N-sessions shape only ever sketched, dead and
     * commented-out, in core/include/euclid/core/LogStream.h's WebSocketSessionManager.
     *
     * Sessions are stored as weak_ptr and never explicitly deregistered: a session whose
     * shared_ptr chain has already ended (connection closed) simply fails to lock() and is
     * pruned the next time Broadcast() sweeps its bucket - simpler than wiring an explicit
     * deregistration call into every session's teardown path, and correct because Broadcast()
     * always visits the whole bucket anyway.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class GatewayWsRegistry {
    public:
        [[nodiscard]]
        static GatewayWsRegistry &instance();

        /**
         * @brief Registers a session for event delivery under the given accountId/region.
         *
         * @param accountId account the session authenticated for
         * @param region    region the session authenticated for
         * @param session   the session itself
         */
        void Register(const std::string &accountId, const std::string &region, const std::weak_ptr<IWsSession> &session);

        /**
         * @brief Delivers a pushed event to the sessions attached to one EES subscriber name.
         *
         * @par
         * What belongs to a named subscription was decided when the event was published - its
         * filter and its account were evaluated there - so a session that attached to the name
         * receives what the subscription matched, not what the connection separately asked for.
         *
         * @par
         * Every attached session is delivered to, rather than one chosen among them: two
         * instances sharing a name are meant to share the work, and the arbiter for that is the
         * atomic claim in Database::EventBus, not this fan-out. For a durable subscription the
         * frame carries the fact that events are waiting and the instances race to claim them;
         * exactly one wins each event.
         *
         * @param subscriber EES subscriber name.
         * @param topic      event topic.
         * @param accountId  account the event belongs to.
         * @param region     region the event belongs to.
         * @param body       event payload.
         * @return number of sessions the event was delivered to.
         */
        long DeliverToSubscriber(const std::string &subscriber, const std::string &topic, const std::string &accountId,
                                 const std::string &region, const boost::json::object &body);

    private:
        [[nodiscard]]
        static std::string key(const std::string &accountId, const std::string &region);

        std::mutex _mutex;
        std::unordered_map<std::string, std::vector<std::weak_ptr<IWsSession> > > _sessionsByScope;
    };

}// namespace Euclid::main
