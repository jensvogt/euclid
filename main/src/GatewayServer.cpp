// C++ includes
#include <algorithm>
#include <charconv>
#include <optional>

// Boost includes
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/ssl.hpp>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/SigV4.h>
#include <euclid/core/monitoring/MonitoringTimer.h>
#include <euclid/manager/GatewayServer.h>
#include <euclid/manager/GatewayWsSession.h>

namespace Euclid::main {

    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace websocket = beast::websocket;
    namespace asio = boost::asio;
    using tcp = asio::ip::tcp;

    // Whether the gateway accepts websocket upgrades at all - lets ops disable the transport
    // without a rebuild. Checked per-upgrade-attempt rather than cached, same cost as any other
    // Configuration::getOr() call elsewhere in this file.
    static bool WebSocketEnabled() {
        return Core::Configuration::instance().getOr<bool>("euclid.gateway.websocket.enabled", true);
    }

    // Boost.Beast's parser defaults to a 1MB body limit when none is set explicitly, which is
    // silently exceeded by a single storage upload-part (default part size alone is 5MB) - the
    // parser then fails the read and the session tears down with no response, surfacing to the
    // client as a bare "connection reset". euclid.gateway.http.max-body already exists in the
    // shipped configs for this; it just wasn't wired to anything, so this is what makes it real.
    static std::uint64_t MaxBodySize() {
        constexpr long kDefaultMaxBodySize = 512L * 1024 * 1024;
        return static_cast<std::uint64_t>(Core::Configuration::instance().getOr<long>("euclid.gateway.http.max-body", kDefaultMaxBodySize));
    }

    // How long forwardToService() waits on the module before giving up. Matters most when the
    // autoscaler kills an instance mid-request (or any other backend hiccup): without a bound
    // here, that single request can wedge a gateway worker thread forever, and enough of those
    // pile up until the whole gateway - not just the module the stuck request happened to hit -
    // stops making progress on anything.
    static std::chrono::seconds BackendTimeout() {
        constexpr long kDefaultBackendTimeoutSeconds = 60;
        return std::chrono::seconds(Core::Configuration::instance().getOr<long>("euclid.gateway.http.backend-timeout-seconds", kDefaultBackendTimeoutSeconds));
    }

    // ── Euclid service detection ────────────────────────────────────────────────

    // Returns the lowercase Euclid service name for a request, or empty string if
    // this is not an Euclid service call.
    //
    // Detection order:
    //   1. x-euclid-target — "DynamoDB_20120810.PutItem" → "dynamodb"
    //   2. Authorization   — SigV4 credential scope: "<key>/<date>/<region>/<svc>/aws4_request"
    static std::string detectEuclidService(const http::request<http::string_body> &req) {
        static const std::unordered_set<std::string> kModules{
                "eam", "esm", "eqs", "ens", "emm", "emo", "ekm", "ets"
        };

        if (const auto module = std::string(req["x-euclid-target"]); !module.empty()) {
            log_debug << "Euclid service call detected from x-euclid-target: " << module;
            if (kModules.contains(module)) return module;
        }

        if (const auto auth = Core::SigV4::ParseAuthorizationHeader(std::string(req[http::field::authorization])); auth.has_value()) {
            log_debug << "Euclid service call detected from SigV4 credential scope: " << auth->scope.service;
            if (kModules.contains(auth->scope.service)) return auth->scope.service;
        }
        return {};
    }

    // Service:action pairs reachable without a bearer token - login has no token to present
    // yet, and register's own admin-vs-bootstrap check happens downstream in the access module
    // (it needs to know whether the user store is empty, which only that module can see).
    static bool isPublicAction(const std::string &service, const std::string &action) {
        static const std::unordered_set<std::string> kPublic{"eam:login", "eam:register"};
        return kPublic.contains(service + ":" + action);
    }

    // Proxies req over a Unix-domain socket at socketPath and returns the
    // backend's response.  Runs synchronously on the calling worker thread.
    // Declared in GatewayServer.h so GatewayWsSession.cpp can reuse it too.
    http::response<http::string_body> forwardToService(const http::request<http::string_body> &req, const std::string &socketPath) {
        namespace local = asio::local;
        auto errorResponse = [&](const http::status st, const std::string_view msg) {
            http::response<http::string_body> r{st, req.version()};
            r.set(http::field::content_type, "application/json");
            r.keep_alive(req.keep_alive());
            r.body() = boost::json::serialize(boost::json::object{{"error", msg}});
            r.prepare_payload();
            return r;
        };

        if (req.method() == http::verb::options) {
            http::response<http::string_body> r{http::status::no_content, req.version()};
            r.set(http::field::allow, "GET, POST, PUT, DELETE, PATCH, OPTIONS");
            r.set("Access-Control-Allow-Origin", "*");
            r.set("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, PATCH, OPTIONS");
            r.set("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With");
            r.set("Access-Control-Max-Age", "86400");
            r.keep_alive(req.keep_alive());
            r.prepare_payload();
            return r;
        }

        // Chained as a single async pipeline (connect -> write -> read) under one ioc.run() so
        // BackendTimeout() actually bounds the whole exchange - beast::basic_stream's timeout
        // support, like tcp_stream's, only takes effect for asynchronous operations. The
        // synchronous connect/write/read this replaced ignored the timeout entirely and could
        // hang the calling gateway worker thread forever on a backend that never responds.
        try {
            asio::io_context ioc;
            beast::basic_stream<local::stream_protocol> stream(ioc);
            stream.expires_after(BackendTimeout());

            beast::error_code opEc;
            beast::flat_buffer buf;
            http::response<http::string_body> res;

            stream.async_connect(local::stream_protocol::endpoint(socketPath), [&](const beast::error_code &ec) {
                if (ec) {
                    opEc = ec;
                    return;
                }
                http::async_write(stream, req, [&](const beast::error_code &writeEc, std::size_t) {
                    if (writeEc) {
                        opEc = writeEc;
                        return;
                    }
                    http::async_read(stream, buf, res, [&](const beast::error_code &readEc, std::size_t) {
                        opEc = readEc;
                    });
                });
            });

            ioc.run();

            if (opEc) return errorResponse(http::status::bad_gateway, opEc.message());
            return res;
        } catch (const std::exception &ex) {
            return errorResponse(http::status::bad_gateway, ex.what());
        }
    }

    // ── Router ───────────────────────────────────────────────────────────────
    // Shared by both GatewaySession and GatewayTlsSession - routing doesn't depend
    // on the underlying stream type.

    static http::response<http::string_body> route(const http::request<http::string_body> &req, ServiceController &ctrl) {

        // Labelled by HTTP method rather than per-action (unlike each module's own
        // "<module>-service-time"/"-service-count", recorded per action inside that module) -
        // this is the single chokepoint every request through the gateway passes through,
        // regardless of target service, and at this point route() hasn't parsed out an
        // Euclid action yet (nor does every request through here carry one, e.g. a bare
        // OPTIONS preflight) - the HTTP method is the one dimension always available up front.
        Core::Monitoring::MonitoringTimer measure("gateway-service-time", "gateway-service-count", "method", std::string(req.method_string()));

        const auto version = req.version();
        const auto keepAlive = req.keep_alive();

        // Strip query string from target
        std::string target(req.target());
        if (const auto q = target.find('?'); q != std::string::npos) target.resize(q);

        auto jsonResponse = [&](const http::status status, std::string body) {
            http::response<http::string_body> r{status, version};
            r.set(http::field::content_type, "application/json");
            r.keep_alive(keepAlive);
            r.body() = std::move(body);
            r.prepare_payload();
            return r;
        };

        auto ok = [&](std::string body) {
            return jsonResponse(http::status::ok, std::move(body));
        };
        auto err = [&](const http::status status, std::string_view msg) {
            return jsonResponse(status, boost::json::serialize(boost::json::object{{"error", msg}}));
        };

        // A single unhandled exception anywhere below (auth, JSON parsing, forwarding, ...) must
        // not escape this function: it would propagate out of the async_read completion handler
        // that called route(), out of the io_context::run() on whichever gateway worker thread
        // happened to be running it, and - since an exception escaping a std::thread's entry
        // function calls std::terminate() - take down the entire manager process, every module it
        // manages included. Converting it into a 500 here costs one failed request instead of the
        // whole gateway. Mirrors the same guard UnixSocketServer::Session::doRead() already has
        // around Dispatch() for individual module processes.
        try {
            // ── Euclid service dispatch ─────────────────────────────────────────
            if (const auto service = detectEuclidService(req); !service.empty()) {
                const auto action = std::string(req["x-euclid-action"]);
                if (!isPublicAction(service, action)) {
                    const auto auth = Core::HttpActionServer::Authenticate(req);
                    if (!auth.subject.has_value()) {
                        return Core::HttpActionServer::Unauthorized(req, auth);
                    }
                }

                // Lets a client proactively declare concurrency it's about to need (e.g. the
                // storage CLI's upload-file --concurrency, sent on create-upload) so the autoscaler
                // can ramp toward it directly instead of waiting to sample busy instances - see
                // ServiceController::declareExpectedConcurrency()'s doc comment. Generic at the
                // gateway level (any module's requests can carry it), not storage-specific.
                if (const auto concurrency = req["x-euclid-expected-concurrency"]; !concurrency.empty()) {
                    if (int desired = 0; std::from_chars(concurrency.data(), concurrency.data() + concurrency.size(), desired).ec == std::errc{} && desired > 0) {
                        if (!ctrl.declareExpectedConcurrency(service, desired)) {
                            return err(http::status::bad_request, "requested concurrency (" + std::to_string(desired) + ") exceeds configured max instances for service '" + service + "'");
                        }
                    }
                }

                const auto handle = ctrl.acquireInstance(service);
                if (!handle) return err(http::status::service_unavailable, "service '" + service + "' not registered or not running");

                // Guarantees the autoscaler's active-request count is released on every exit path
                // below, however forwardToService() returns.
                struct ReleaseGuard {
                    ServiceController &ctrl;
                    const std::string &name;
                    pid_t pid;
                    ~ReleaseGuard() { ctrl.releaseInstance(name, pid); }
                } guard{.ctrl = ctrl, .name = service, .pid = handle->pid};

                return forwardToService(req, handle->socketPath);
            }

            return err(http::status::not_found, "not found");
        } catch (const std::exception &ex) {
            log_error << "route() threw, error: " << ex.what();
            return err(http::status::internal_server_error, ex.what());
        } catch (...) {
            log_error << "route() threw a non-std::exception";
            return err(http::status::internal_server_error, "internal server error");
        }
    }

    // ── GatewaySession ───────────────────────────────────────────────────────
    // Handles one plaintext TCP connection: reads requests in a loop (keep-alive
    // aware), dispatches to the router, and writes responses.

    class GatewaySession : public std::enable_shared_from_this<GatewaySession> {
    public:

        GatewaySession(tcp::socket socket, asio::io_context &ioc, ServiceController &ctrl) : _stream(std::move(socket)), _ioc(ioc), _ctrl(ctrl) {}

        void run() { doRead(); }

    private:

        void doRead() {
            _parser.emplace();
            _parser->body_limit(MaxBodySize());
            _stream.expires_after(std::chrono::seconds(30));
            http::async_read(
                    _stream, _buf, *_parser,
                    [self = shared_from_this()](const beast::error_code &ec, std::size_t) {
                        if (!ec) self->handleRequest();
                        // on error (EOF, timeout, body_limit exceeded, etc.) the session destructs naturally
                    });
        }

        void handleRequest() {
            auto &req = _parser->get();

            // Hands the connection off to a GatewayWsSession permanently - this object's
            // shared_ptr chain ends once handleUpgrade() returns, since nothing keeps posting
            // further work to it. See GatewayWsSession.h's class doc comment for the protocol
            // and the auth-once-at-handshake tradeoff this makes.
            if (WebSocketEnabled() && websocket::is_upgrade(req)) {
                handleUpgrade();
                return;
            }

            auto res = route(req, _ctrl);
            auto sp = std::make_shared<http::response<http::string_body> >(std::move(res));
            const bool keepAlive = sp->keep_alive();
            http::async_write(
                    _stream, *sp,
                    [self = shared_from_this(), sp, keepAlive](const beast::error_code &ec, std::size_t) {
                        if (!ec && keepAlive) self->doRead();
                        else {
                            beast::error_code ignored;
                            std::ignore = self->_stream.socket().shutdown(tcp::socket::shutdown_send, ignored);
                        }
                    });
        }

        void handleUpgrade() {
            auto &req = _parser->get();

            // Authenticated exactly once, here, at handshake time - reuses the same SigV4/Bearer
            // verification the HTTP path runs per-request. On failure the connection is closed
            // without ever completing the websocket handshake.
            const auto auth = Core::HttpActionServer::Authenticate(req);
            if (!auth.subject.has_value()) {
                auto res = Core::HttpActionServer::Unauthorized(req, auth);
                auto sp = std::make_shared<http::response<http::string_body> >(std::move(res));
                http::async_write(_stream, *sp, [self = shared_from_this(), sp](const beast::error_code &, std::size_t) {
                    beast::error_code ignored;
                    std::ignore = self->_stream.socket().shutdown(tcp::socket::shutdown_send, ignored);
                });
                return;
            }

            const auto authorization = std::string(req[http::field::authorization]);
            const auto accountId = std::string(req["x-euclid-account-id"]);
            const auto region = std::string(req["x-euclid-region"]);
            const auto ns = std::string(req["x-euclid-namespace"]);
            auto session = std::make_shared<GatewayWsSession>(std::move(_stream), _ioc, _ctrl, authorization, accountId, region, ns);
            session->run(std::move(req));
        }

        beast::tcp_stream _stream;
        beast::flat_buffer _buf;
        std::optional<http::request_parser<http::string_body> > _parser;
        asio::io_context &_ioc;
        ServiceController &_ctrl;
    };

    // ── GatewayTlsSession ────────────────────────────────────────────────────
    // Same request loop as GatewaySession, but the connection is terminated with
    // TLS: a handshake runs before the first read, and the stream is an SSL layer
    // wrapped around the TCP stream.

    class GatewayTlsSession : public std::enable_shared_from_this<GatewayTlsSession> {
    public:

        GatewayTlsSession(tcp::socket socket, asio::ssl::context &sslCtx, asio::io_context &ioc, ServiceController &ctrl)
            : _stream(std::move(socket), sslCtx), _ioc(ioc), _ctrl(ctrl) {}

        void run() {
            beast::get_lowest_layer(_stream).expires_after(std::chrono::seconds(30));
            _stream.async_handshake(
                    asio::ssl::stream_base::server,
                    [self = shared_from_this()](const beast::error_code &ec) {
                        if (!ec) self->doRead();
                        // handshake failure: session destructs naturally
                    });
        }

    private:

        void doRead() {
            _parser.emplace();
            _parser->body_limit(MaxBodySize());
            beast::get_lowest_layer(_stream).expires_after(std::chrono::seconds(30));
            http::async_read(
                    _stream, _buf, *_parser,
                    [self = shared_from_this()](const beast::error_code &ec, std::size_t) {
                        if (!ec) self->handleRequest();
                        // on error (EOF, timeout, body_limit exceeded, etc.) the session destructs naturally
                    });
        }

        void handleRequest() {
            auto &req = _parser->get();

            if (WebSocketEnabled() && websocket::is_upgrade(req)) {
                handleUpgrade();
                return;
            }

            auto res = route(req, _ctrl);
            auto sp = std::make_shared<http::response<http::string_body> >(std::move(res));
            const bool keepAlive = sp->keep_alive();
            http::async_write(
                    _stream, *sp,
                    [self = shared_from_this(), sp, keepAlive](const beast::error_code &ec, std::size_t) {
                        if (!ec && keepAlive) self->doRead();
                        else {
                            // Deliberately closes the raw socket rather than calling the
                            // stream's shutdown() (SSL "close_notify" handshake): that overload
                            // is *synchronous* and, unlike every other operation here, isn't
                            // covered by expires_after() - it blocks waiting for the peer to
                            // send its own close_notify back, and a client that's already done
                            // with the connection (e.g. one that never reuses connections, like
                            // euclid-cli) has no reason to send one. Since this runs on a
                            // gateway worker thread shared by every other in-flight connection
                            // (see BackendTimeout()'s doc comment on the same hazard for
                            // forwardToService()), a single hung shutdown() here would stall the
                            // whole gateway, not just this session.
                            beast::get_lowest_layer(self->_stream).close();
                        }
                    });
        }

        void handleUpgrade() {
            auto &req = _parser->get();

            const auto auth = Core::HttpActionServer::Authenticate(req);
            if (!auth.subject.has_value()) {
                auto res = Core::HttpActionServer::Unauthorized(req, auth);
                auto sp = std::make_shared<http::response<http::string_body> >(std::move(res));
                http::async_write(_stream, *sp, [self = shared_from_this(), sp](const beast::error_code &, std::size_t) {
                    beast::get_lowest_layer(self->_stream).close();
                });
                return;
            }

            const auto authorization = std::string(req[http::field::authorization]);
            const auto accountId = std::string(req["x-euclid-account-id"]);
            const auto region = std::string(req["x-euclid-region"]);
            const auto ns = std::string(req["x-euclid-namespace"]);
            auto session = std::make_shared<GatewayWsTlsSession>(std::move(_stream), _ioc, _ctrl, authorization, accountId, region, ns);
            session->run(std::move(req));
        }

        beast::ssl_stream<beast::tcp_stream> _stream;
        beast::flat_buffer _buf;
        std::optional<http::request_parser<http::string_body> > _parser;
        asio::io_context &_ioc;
        ServiceController &_ctrl;
    };

    // ── GatewayServer ────────────────────────────────────────────────────────

    GatewayServer::GatewayServer(ServiceController &ctrl, const unsigned short port, const int threads) : _ctrl(ctrl), _ioc(threads), _acceptor(_ioc), _port(port), _threads(threads) {

        const tcp::endpoint endpoint{tcp::v6(), port};
        _acceptor.open(endpoint.protocol());
        _acceptor.set_option(asio::socket_base::reuse_address(true));
        _acceptor.set_option(asio::ip::v6_only(false));
        _acceptor.bind(endpoint);
        _acceptor.listen(asio::socket_base::max_listen_connections);

        _tlsEnabled = Core::Configuration::instance().getOr<bool>("euclid.gateway.tls.enabled", false);
        if (_tlsEnabled) {
            const auto certFile = Core::Configuration::instance().getOr<std::string>("euclid.gateway.tls.cert-file", "");
            const auto keyFile = Core::Configuration::instance().getOr<std::string>("euclid.gateway.tls.key-file", "");
            if (certFile.empty() || keyFile.empty()) {
                throw std::runtime_error("euclid.gateway.tls.enabled is true but cert-file/key-file are not configured");
            }
            _sslCtx.use_certificate_chain_file(certFile);
            _sslCtx.use_private_key_file(keyFile, asio::ssl::context::pem);
        }
    }

    GatewayServer::~GatewayServer() { stop(); }

    void GatewayServer::start() {
        doAccept();
        _workers.reserve(_threads);
        for (int i = 0; i < _threads; ++i) _workers.emplace_back([this] { _ioc.run(); });
        log_info << "Gateway " << (_tlsEnabled ? "HTTPS" : "HTTP") << " server listening on port " << _port << " (" << _threads << " worker thread(s))";
    }

    void GatewayServer::stop() {
        _acceptor.close();
        _ioc.stop();
        for (auto &t: _workers) if (t.joinable()) t.join();
        _workers.clear();
    }

    void GatewayServer::doAccept() {
        _acceptor.async_accept(asio::make_strand(_ioc), [this](const beast::error_code &ec, tcp::socket socket) {
            if (!ec) {
                if (_tlsEnabled) std::make_shared<GatewayTlsSession>(std::move(socket), _sslCtx, _ioc, _ctrl)->run();
                else std::make_shared<GatewaySession>(std::move(socket), _ioc, _ctrl)->run();
            }
            if (!_ioc.stopped()) doAccept();
        });
    }

}// namespace Euclid::main