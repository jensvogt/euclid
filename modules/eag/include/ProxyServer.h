//
// Created by vogje01 on 9/5/26.
//

#pragma once

// C++ includes
#include <atomic>
#include <string>
#include <thread>
#include <vector>

// Boost includes
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <memory>

// Euclid includes
#include <BasicAuthenticator.h>
#include <Backends.h>
#include <RouteTable.h>

namespace Euclid::EAG {

    /**
     * @brief The API gateway's own listener: the port callers actually talk to.
     *
     * @par
     * Separate from the Unix socket the manager supervises this process through, in the same way a
     * transfer server has both. The socket carries euclid's own traffic - the route table is
     * managed through it - and this carries everybody else's.
     *
     * @par
     * A request arriving here is matched against the route table by path, authenticated if its
     * route asks for it, and forwarded to one of the instances of the application that route
     * names. The caller never learns which instance answered, and the application never learns it
     * was proxied: the path it receives is the path that was asked for.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class ProxyServer {

    public:

        /**
         * @brief Binds the listener and starts serving.
         *
         * @param port TCP port to listen on.
         * @param threads io_context worker threads.
         * @param refreshSeconds how often the route table and backend list are re-read.
         */
        /**
         * @brief One port the gateway answers on, and the namespace its routes belong to.
         *
         * @par
         * Several of them is how one installation serves more than one environment without the
         * environment appearing in anybody's URL: development on 8080 and integration on 8081 can
         * both publish "/api/produktmeldungen", because the port says which is meant. The
         * alternative - one port and the namespace in the path - puts euclid's own structure into
         * addresses that outlive it, and makes the application see a segment that is nothing to do
         * with it.
         */
        struct Listener {

            /**
             * @brief TCP port to answer on.
             */
            unsigned short port{};

            /**
             * @brief Namespace whose routes this port serves. Empty serves every route, which is
             * what a single-listener installation gets.
             */
            std::string nameSpace;
        };

        ProxyServer(std::vector<Listener> listeners, int threads, long refreshSeconds, long basicAuthCacheSeconds,
                    unsigned short euclidGatewayPort, bool euclidGatewayTls, const std::string &euclidGatewayCert);

        ~ProxyServer();

        ProxyServer(const ProxyServer &) = delete;
        ProxyServer &operator=(const ProxyServer &) = delete;

        /**
         * @brief Starts accepting, and the refresh loop that keeps the routes current.
         */
        void start();

        /**
         * @brief Stops accepting and joins the workers.
         */
        void stop();

    private:

        /**
         * @brief Accepts one connection and re-arms itself.
         */
        void accept(std::size_t index);

        /**
         * @brief Serves one connection: read a request, route it, answer it.
         */
        void serve(boost::asio::ip::tcp::socket socket, const std::string &nameSpace);

        /**
         * @brief Matches one request to a route, authenticates it if the route says so, and
         * forwards it to one of the application's instances.
         */
        void route(const std::string &nameSpace,
                   const std::shared_ptr<boost::beast::tcp_stream> &stream,
                   const std::shared_ptr<boost::beast::http::request<boost::beast::http::string_body> > &request);

        /**
         * @brief Forwards one request to a backend port and returns whatever comes back.
         */
        void proxyTo(const std::shared_ptr<boost::beast::tcp_stream> &stream,
                     const std::shared_ptr<boost::beast::http::request<boost::beast::http::string_body> > &request,
                     int port, const std::string &routeId,
                     const std::string &euclidTarget, const std::string &euclidAction);

        /**
         * @brief Proxies to euclid's own gateway over TLS.
         *
         * @par
         * Separate from proxyTo() rather than a flag on it, because the stream type differs all
         * the way down and there is a handshake in the middle. Used when
         * euclid.gateway.tls.enabled is set, which it is by default - a module route that spoke
         * plain HTTP to it would be answered with a dropped connection and nothing else.
         */
        void proxyToTls(const std::shared_ptr<boost::beast::tcp_stream> &stream,
                        const std::shared_ptr<boost::beast::http::request<boost::beast::http::string_body> > &request,
                        int port, const std::string &routeId,
                        const std::string &euclidTarget, const std::string &euclidAction);

        /**
         * @brief Writes one response to the caller and closes the connection.
         */
        static void respond(const std::shared_ptr<boost::beast::tcp_stream> &stream,
                            const std::shared_ptr<boost::beast::http::response<boost::beast::http::string_body> > &response);

        /**
         * @brief Re-reads the route table, then the backends of whatever it now names.
         *
         * @par
         * In that order and in one place, so the two can never disagree about which applications
         * matter: a route added a moment ago has its instances looked up in the same pass.
         */
        void refresh();

        std::vector<Listener> _listeners;
        int _threads;
        std::chrono::seconds _refreshInterval;

        boost::asio::io_context _ioc;

        /**
         * @brief One acceptor per listener, in the same order, so an accepted connection knows
         * which namespace it arrived for.
         */
        std::vector<boost::asio::ip::tcp::acceptor> _acceptors;
        std::vector<std::thread> _workers;
        std::thread _refreshThread;
        std::atomic<bool> _running{false};

        RouteTable _routes;
        Backends _backends;

        /**
         * @brief Checks Basic credentials for the routes that ask for them.
         */
        BasicAuthenticator _basicAuth;

        /**
         * @brief Port euclid's own gateway listens on, where a route naming a module is sent.
         */
        unsigned short _euclidGatewayPort;

        /**
         * @brief Whether euclid's own gateway expects TLS. Mirrors euclid.gateway.tls.enabled.
         */
        bool _euclidGatewayTls;

        /**
         * @brief The installation's region, supplied for callers that do not name one.
         */
        std::string _region;

        /**
         * @brief Client context for talking to it, built once because building one per request
         * would re-read and re-parse the certificate every time.
         */
        boost::asio::ssl::context _euclidGatewayCtx;
    };

}// namespace Euclid::EAG
