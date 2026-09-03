#pragma once

// C++ includes
#include <algorithm>
#include <memory>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

// Boost includes
#include <boost/json.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/v6_only.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

// Euclid includes
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/HttpUtils.h>
#include <euclid/core/LogStream.h>
#include <euclid/database/entity/emm/ModuleState.h>
#include <euclid/manager/Controller.h>

namespace Euclid::main {

    /**
     * @brief Proxies req over a Unix-domain socket at socketPath and returns the backend's
     * response. Runs synchronously on the calling thread, bounded by
     * euclid.gateway.http.backend-timeout-seconds.
     *
     * Shared by both the HTTP request path (this file's route()) and the websocket request path
     * (GatewayWsSession.cpp/GatewayWsTlsSession), so a backend module never has to know or care
     * which transport the original client used - see WsFrame::ToHttpRequest(), which builds the
     * same http::request shape from a websocket frame that route() builds from an actual HTTP
     * request.
     */
    [[nodiscard]]
    boost::beast::http::response<boost::beast::http::string_body>
    forwardToService(const boost::beast::http::request<boost::beast::http::string_body> &req, const std::string &socketPath);

    /**
     * @brief What a routed request is answered through, rather than by returning.
     */
    using RouteCompletion = std::function<void(boost::beast::http::response<boost::beast::http::string_body>)>;

    /**
     * @brief Proxies req over a Unix-domain socket at socketPath and hands the backend's response
     * to done, without holding the calling thread while the backend produces it.
     *
     * The HTTP request path uses this so that waiting on a module costs memory rather than a
     * worker thread. Forwarding synchronously put a ceiling on the gateway equal to its worker
     * count (euclid.gateway.http.max-thread), whatever the modules behind it could handle - and
     * long-polling actions, which deliberately wait up to twenty seconds, each held one of those
     * threads for the whole wait.
     *
     * @param ioc the io_context the exchange runs on - the gateway's own, so its worker threads
     *            pick the continuations up as they become runnable
     * @param req the request to forward; must stay alive until done is called
     * @param socketPath the module instance's Unix domain socket
     * @param done called exactly once, with the backend's response or a 502 describing why there
     *             is none
     */
    void forwardToServiceAsync(boost::asio::io_context &ioc,
                               const boost::beast::http::request<boost::beast::http::string_body> &req,
                               const std::string &socketPath, RouteCompletion done);

    /**
     * @brief Represents a server that manages gateway connections.
     *
     * This class initializes the server, configures the necessary resources, and
     * prepares the environment for handling client gateway interactions.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class GatewayServer {
    public:
        /**
         * @brief Constructs the gateway server.
         *
         * Opens the TCP acceptor but does not start accepting connections until
         * start() is called.
         *
         * @param ctrl    Reference to the service controller used to fulfil requests.
         * @param port    TCP port to listen on.
         * @param threads Number of io_context worker threads.
         */
        GatewayServer(ServiceController &ctrl, unsigned short port, int threads = 2);

        /**
         * @brief Destructor
         */
        ~GatewayServer();

        /**
         * @brief Starts the acceptance loop and io_context worker threads.
         *
         * Returns immediately; work runs on background threads.
         */
        void start();

        /**
         * @brief Stops the server and joins all worker threads.
         */
        void stop();

    private:
        /**
         *
         */
        void doAccept();

        /**
         * @brief Reference to the service controller used by the GatewayServer.
         *
         * This member provides access to the ServiceController, which is responsible
         * for handling and fulfilling service-related requests. The GatewayServer uses
         * this reference to delegate request processing and manage service operations.
         */
        ServiceController &_ctrl;

        /**
         * @brief Provides the I/O context for managing asynchronous operations.
         *
         * This member is used as the core mechanism to manage asynchronous input/output
         * operations throughout the server. It is responsible for scheduling tasks,
         * running I/O handlers, and coordinating asynchronous communications.
         *
         * The `_ioc` object operates as the central service execution context for initiating
         * and handling asynchronous tasks, such as accepting incoming connections or processing I/O events.
         */
        boost::asio::io_context _ioc;

        /**
         * @brief Manages incoming TCP connections for the gateway server.
         *
         * The TCP acceptor is responsible for setting up the server to listen for
         * incoming client connections. It binds to the specified endpoint and configures
         * the socket to accept and manage connections within the server.
         */
        boost::asio::ip::tcp::acceptor _acceptor;

        /**
         * @brief Whether incoming connections are terminated with TLS.
         *
         * Read from euclid.gateway.tls.enabled at construction time. When true, _sslCtx is
         * loaded with the configured certificate/key and doAccept() hands sockets to a
         * GatewayTlsSession instead of a plain GatewaySession.
         */
        bool _tlsEnabled{false};

        /**
         * @brief TLS context used for the server-side handshake when _tlsEnabled is true.
         *
         * Loaded from euclid.gateway.tls.cert-file / key-file. Unused (default-constructed,
         * no certificate loaded) when TLS is disabled.
         */
        boost::asio::ssl::context _sslCtx{boost::asio::ssl::context::tlsv12_server};

        /**
         * @brief Container for managing worker threads executing asynchronous tasks.
         *
         * This vector holds the collection of threads that run the `io_context` to handle
         * asynchronous operations such as network I/O and connection management. Each thread
         * in this container operates independently, enabling concurrent processing of multiple
         * tasks to ensure efficient server performance. Threads are initialized and managed
         * during the server's runtime lifecycle, starting with the server's initialization
         * and cleaned up upon shutdown.
         */
        std::vector<std::thread> _workers;

        /**
         * @brief Specifies the TCP port on which the server listens for incoming connections.
         *
         * This variable defines the port number used by the GatewayServer for
         * establishing client connections. It is initialized during the construction
         * of the server and remains constant throughout the server's lifecycle.
         */
        unsigned short _port;

        /**
         * @brief Specifies the number of worker threads for the io_context.
         *
         * This variable determines the level of concurrency for handling network
         * operations. It configures the number of threads that will process I/O
         * events in the server, ensuring effective resource utilization and managing
         * concurrent client connections efficiently.
         */
        int _threads;
    };

} // namespace Euclid::main