#pragma once

// C++ includes
#include <string>

// Boost includes
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

// Euclid includes
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/LogStream.h>
#include <euclid/database/RepositoryFactory.h>

namespace Euclid::EAP {

    using namespace boost::beast::http;

    /**
     * @brief Application module - the control plane for applications euclid runs.
     *
     * @par
     * EAP never runs an application itself. It owns the definitions - which artifact in which
     * bucket, which runtime starts it, which EAM user it runs as, how far the autoscaler may
     * scale it - and start/stop are nothing more than writing a desired state onto one of those
     * definitions. euclid-mgr's reconciler is what turns that into running processes: it
     * materialises the artifact out of ESM, hands the process its credentials through the
     * environment, and from then on treats it exactly like any other module in the pool.
     *
     * @par
     * The application itself needs no euclid library, no database access and no knowledge of any
     * of this - which is what lets it be written in Java, Python, Node.js, Rust or C++. All it
     * has to do is listen on the socket path it is given in EUCLID_SOCKET.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class EapServer final : public Core::HttpActionServer {

    public:

        /**
         * @brief Constructs the server.
         *
         * @param socketPath Unix domain socket path to listen on.
         * @param threads    number of io_context worker threads.
         */
        explicit EapServer(std::string socketPath, int threads = 2);

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

}// namespace Euclid::EAP
