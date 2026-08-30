#pragma once

// C++ includes
#include <string>

// Euclid includes
#include <euclid/core/UnixSocketServer.h>

namespace Euclid::main {

    /**
     * @brief Receiving end of Core::EventPusher, listening on euclid.gateway.event-socket-path -
     * mirrors how the "emo" module's push-metrics handler is the receiving end of
     * Core::Monitoring::MetricsPusher.
     *
     * Every module process can reach this over its own fixed, configured Unix domain socket path
     * (the gateway is a single process, not a scalable pool like modules are, so - unlike
     * MetricsPusher's dynamic module-repository lookup for "emo" - there's no discovery step:
     * every module just reads the same config key). Not authenticated, same as push-metrics: only
     * reachable from another local process over a filesystem-permissioned Unix socket, the same
     * trust boundary every other module-to-module call in this codebase already relies on.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class GatewayEventIngest final : public Core::UnixSocketServer {
    public:
        /**
         * @param socketPath Unix domain socket path to listen on (euclid.gateway.event-socket-path).
         * @param threads    number of io_context worker threads.
         */
        explicit GatewayEventIngest(std::string socketPath, int threads = 2);

    protected:
        [[nodiscard]]
        boost::beast::http::response<boost::beast::http::string_body>
        Dispatch(const boost::beast::http::request<boost::beast::http::string_body> &req) override;
    };

}// namespace Euclid::main
