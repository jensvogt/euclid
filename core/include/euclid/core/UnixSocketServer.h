#pragma once

// C++ includes
#include <string>
#include <thread>
#include <vector>

// Boost includes
#include <boost/asio/io_context.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

namespace Euclid::Core {

    /**
     * @brief Base class for module services that listen on a Unix domain socket and speak
     * HTTP, e.g. the access and SQS services.
     *
     * Owns the accept loop, worker threads and socket lifecycle. Subclasses only need to
     * implement Dispatch() to turn a request into a response.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class UnixSocketServer {

    public:

        /**
         * @brief Constructs the server and binds the Unix domain socket.
         *
         * @param serviceName Human-readable service name, used in log messages, e.g. "Access"
         * @param socketPath  Unix domain socket path to listen on
         * @param threads     Number of io_context worker threads
         */
        UnixSocketServer(std::string serviceName, std::string socketPath, int threads = 2);

        virtual ~UnixSocketServer() = default;

        UnixSocketServer(const UnixSocketServer &) = delete;
        UnixSocketServer &operator=(const UnixSocketServer &) = delete;

        /**
         * @brief Starts the accept loop and worker threads.
         *
         * Returns immediately; work runs on background threads.
         */
        void start();

        /**
         * @brief Stops the server and joins all worker threads.
         */
        void stop();

        /**
         * @brief Installs SIGTERM/SIGINT handlers, starts the server, blocks the calling
         * thread until one of those signals arrives, then stops the server.
         *
         * Intended to be called directly from a module's main().
         *
         * @return process exit code (always 0)
         */
        int RunUntilSignal();

    protected:

        /**
         * @brief Handles one HTTP request and returns the response. Called on an io_context
         * worker thread for every request received on the socket.
         *
         * @param req request received on the Unix domain socket
         * @return response to send back
         */
        [[nodiscard]]
        virtual boost::beast::http::response<boost::beast::http::string_body>
        Dispatch(const boost::beast::http::request<boost::beast::http::string_body> &req) = 0;

    private:

        class Session;

        /**
         * @brief Initiates an asynchronous acceptance loop for incoming client connections.
         */
        void doAccept();

        static void onSignal(int);

        /**
         * @brief Service name used in log messages, e.g. "Access"
         */
        std::string _serviceName;

        /**
         * @brief Path to the Unix domain socket that the server listens on.
         */
        std::string _socketPath;

        /**
         * @brief Boost.Asio io_context instance used for managing asynchronous operations.
         */
        boost::asio::io_context _ioc;

        /**
         * @brief Accepts incoming client connections on the Unix domain socket.
         */
        boost::asio::local::stream_protocol::acceptor _acceptor;

        /**
         * @brief io_context worker threads.
         */
        std::vector<std::thread> _workers;

        /**
         * @brief Number of io_context worker threads.
         */
        int _threads;

        /**
         * @brief Server instance targeted by onSignal(); set by RunUntilSignal().
         */
        static UnixSocketServer *s_instance;
    };

}
