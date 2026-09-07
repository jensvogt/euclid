//
// Created by vogje01 on 9/5/26.
//

#pragma once

// C++ includes
#include <memory>
#include <string>
#include <vector>

// Boost includes
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

// Euclid includes
#include <ProxyServer.h>
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/LogStream.h>
#include <euclid/database/RepositoryFactory.h>

namespace Euclid::EAG {

    using namespace boost::beast::http;

    /**
     * @brief API gateway module - one HTTP front door for the applications euclid runs.
     *
     * @par
     * Two listeners, and the split is the whole design. This class is the ordinary euclid module:
     * a Unix domain socket the manager supervises it through and the CLI reaches it over, carrying
     * the actions that manage the route table. Alongside it, ProxyServer holds a TCP port that
     * everybody else talks to - a frontend making REST calls, which has no euclid credentials and
     * no business knowing that euclid exists.
     *
     * @par
     * What connects them is configuration rather than convention: a route says that a path belongs
     * to an application, and the gateway resolves that application to whichever of its instances
     * are running at the moment a request arrives. So an application can be scaled, restarted or
     * moved to different ports without anything that calls it noticing, and its name never appears
     * in a URL.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class EagServer final : public Core::HttpActionServer {

    public:

        /**
         * @brief Constructs the module and starts its public listener.
         *
         * @param socketPath Unix domain socket path the manager supervises this process through.
         * @param threads    number of io_context worker threads for that socket.
         */
        explicit EagServer(std::string socketPath, int threads = 4);

        ~EagServer() override;

        EagServer(const EagServer &) = delete;
        EagServer &operator=(const EagServer &) = delete;

        /**
         * @brief Handles one route-management action.
         */
        response<string_body> Dispatch(const request<string_body> &req) override;

    private:

        /**
         * @brief Answers "list-listeners": the ports this gateway answers on, what each speaks,
         * and the certificate an HTTPS one serves.
         *
         * @par
         * A member rather than one of the free handlers beside it, because it is the only action
         * that reports on this process rather than on the route table: the listeners come from
         * the configuration this instance read at start-up, and nothing in the database knows
         * them.
         *
         * @par
         * Read-only and nothing else, deliberately. A port cannot be moved or switched to HTTPS
         * through the API - that is a configuration change and a restart, because a listener that
         * could be re-bound remotely is a way to take an installation off the network with one
         * call.
         */
        [[nodiscard]]
        response<string_body> handleListListeners(const request<string_body> &req) const;

        /**
         * @brief The listeners this instance was configured with, in the order they were read.
         *
         * @par
         * Kept here as well as in the ProxyServer, because a listener that could not be bound has
         * no ProxyServer to be kept in and is exactly the one somebody needs to be shown.
         */
        std::vector<ProxyServer::Listener> _listeners;

        /**
         * @brief The public listener. Held by pointer so a failure to bind its port is reported
         * rather than thrown out of a member's constructor, leaving a module that cannot start
         * but also cannot say why.
         */
        std::unique_ptr<ProxyServer> _proxy;
    };

}// namespace Euclid::EAG
