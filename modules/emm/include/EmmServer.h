#pragma once

// Boost includes
#include <boost/json.hpp>
#include <boost/beast/http.hpp>

// Euclid includes
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/monitoring/MonitoringTimer.h>
#include <euclid/database/RepositoryFactory.h>

namespace Euclid::EMM {

    using namespace boost::beast::http;

    /**
     * @brief EMM (module manager) service server listening on a Unix domain socket.
     *
     * Receives HTTP requests forwarded by the gateway and dispatches them
     * to per-action handler methods. Every action is administrator-only, since it
     * exposes/mutates every module's live process pool and raw database collections.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class EmmServer final : public Core::HttpActionServer {
    public:

        /**
         * @brief Constructs the server.
         *
         * @param socketPath Unix domain socket path to listen on.
         * @param threads    Number of io_context worker threads.
         */
        explicit EmmServer(std::string socketPath, int threads = 2);

        ~EmmServer() override = default;

    protected:

        /**
         * @brief Dispatch the request.
         *
         * @param req HTTP request
         * @return HTTP response
         */
        [[nodiscard]]
        response<string_body> Dispatch(const request<string_body> &req) override;
    };

}// namespace Euclid::EMM
