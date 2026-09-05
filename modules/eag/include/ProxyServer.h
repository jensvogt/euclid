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
#include <boost/beast/http.hpp>
#include <memory>

// Euclid includes
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
        ProxyServer(unsigned short port, int threads, long refreshSeconds);

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
        void accept();

        /**
         * @brief Serves one connection: read a request, route it, answer it.
         */
        void serve(boost::asio::ip::tcp::socket socket);

        /**
         * @brief Matches one request to a route, authenticates it if the route says so, and
         * forwards it to one of the application's instances.
         */
        void route(const std::shared_ptr<boost::beast::tcp_stream> &stream,
                   const std::shared_ptr<boost::beast::http::request<boost::beast::http::string_body> > &request);

        /**
         * @brief Forwards one request to a backend port and returns whatever comes back.
         */
        void proxyTo(const std::shared_ptr<boost::beast::tcp_stream> &stream,
                     const std::shared_ptr<boost::beast::http::request<boost::beast::http::string_body> > &request,
                     int port, const std::string &routeId);

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

        unsigned short _port;
        int _threads;
        std::chrono::seconds _refreshInterval;

        boost::asio::io_context _ioc;
        boost::asio::ip::tcp::acceptor _acceptor;
        std::vector<std::thread> _workers;
        std::thread _refreshThread;
        std::atomic<bool> _running{false};

        RouteTable _routes;
        Backends _backends;
    };

}// namespace Euclid::EAG
