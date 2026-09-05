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
    //   1. x-euclid-target — the module name itself, e.g. "esm"
    //   2. Authorization   — SigV4 credential scope: "<key>/<date>/<region>/<svc>/aws4_request"
    static std::string detectEuclidService(const http::request<http::string_body> &req, const ServiceController &ctrl) {
        static const std::unordered_set<std::string> kModules{
                "eam", "esm", "eqs", "ens", "emm", "emo", "ekm", "ets", "eap", "ees", "eag"
        };

        if (const auto module = std::string(req["x-euclid-target"]); !module.empty()) {
            log_debug << "Euclid service call detected from x-euclid-target: " << module;
            if (kModules.contains(module)) return module;
            // An application is addressed by the name it was deployed under, which cannot be in
            // the list above - only the controller knows which pools exist right now. Asking it
            // is also what keeps this from routing to a name nobody is running: an unknown target
            // is not a euclid service call and falls through to the plain HTTP handling below.
            if (ctrl.hasService(module)) return module;
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

    // Proxies req over a Unix-domain socket at socketPath and hands the backend's response to
    // done, without occupying the calling worker thread while the backend thinks about it.
    // Declared in GatewayServer.h so GatewayWsSession.cpp can reuse it too.
    void forwardToServiceAsync(asio::io_context &ioc, const http::request<http::string_body> &req, const std::string &socketPath, RouteCompletion done) {
        namespace local = asio::local;
        const auto version = req.version();
        const auto keepAlive = req.keep_alive();
        auto errorResponse = [version, keepAlive](const http::status st, const std::string_view msg) {
            http::response<http::string_body> r{st, version};
            r.set(http::field::content_type, "application/json");
            r.keep_alive(keepAlive);
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
            done(std::move(r));
            return;
        }

        // Chained on the GATEWAY'S OWN io_context and completed through a handler, rather than on
        // a private one this function runs to exhaustion. The distinction is the whole point: a
        // private ioc.run() is synchronous however asynchronous its insides are, so the worker
        // thread that called this sat here for as long as the module took to answer. That put a
        // hard ceiling on the gateway equal to its worker count - 64 by default - no matter how
        // many instances of a module were running behind it, and made scaling a module pointless
        // once the gateway itself was the queue.
        //
        // It was also unfairly spent: receive-events and receive-messages deliberately wait up to
        // twenty seconds for something to arrive, so every long-polling consumer parked one of
        // those 64 threads for its whole wait, and a handful of them starved every other module's
        // traffic. Handing the wait to the io_context frees the thread to serve somebody else and
        // leaves only memory - one small state object per request in flight - as the limit.
        //
        // BackendTimeout() still bounds the exchange: beast::basic_stream's timeout support, like
        // tcp_stream's, only ever applied to asynchronous operations anyway.
        try {
            // Held together so the stream, the buffer being read into and the response outlive
            // this function and stay alive for exactly as long as the chain below needs them.
            struct Exchange {
                explicit Exchange(asio::io_context &ioc) : stream(ioc) {}
                beast::basic_stream<local::stream_protocol> stream;
                beast::flat_buffer buf;
                http::response<http::string_body> res;
            };
            auto exchange = std::make_shared<Exchange>(ioc);
            exchange->stream.expires_after(BackendTimeout());

            // req is captured by reference, which is safe for exactly one reason: done owns the
            // session (it captures its shared_ptr), the session owns the parser, and the parser
            // owns req - so every handler below that holds done also keeps req alive.
            exchange->stream.async_connect(
                    local::stream_protocol::endpoint(socketPath),
                    [exchange, &req, done, errorResponse](const beast::error_code &ec) mutable {
                        if (ec) {
                            done(errorResponse(http::status::bad_gateway, ec.message()));
                            return;
                        }
                        http::async_write(
                                exchange->stream, req,
                                [exchange, done, errorResponse](const beast::error_code &writeEc, std::size_t) mutable {
                                    if (writeEc) {
                                        done(errorResponse(http::status::bad_gateway, writeEc.message()));
                                        return;
                                    }
                                    http::async_read(
                                            exchange->stream, exchange->buf, exchange->res,
                                            [exchange, done, errorResponse](const beast::error_code &readEc, std::size_t) mutable {
                                                if (readEc) {
                                                    done(errorResponse(http::status::bad_gateway, readEc.message()));
                                                    return;
                                                }
                                                done(std::move(exchange->res));
                                            });
                                });
                    });
            return;
        } catch (const std::exception &ex) {
            done(errorResponse(http::status::bad_gateway, ex.what()));
        }
    }

    // The websocket path still forwards synchronously: a GatewayWsSession handles one connection's
    // frames in order on its own thread, so there is no shared pool for a wait to starve, and the
    // frame-by-frame flow has nothing to hand a completion to. Implemented on top of the async
    // form so both transports go through exactly one piece of forwarding logic.
    http::response<http::string_body> forwardToService(const http::request<http::string_body> &req, const std::string &socketPath) {
        asio::io_context ioc;
        http::response<http::string_body> out;
        forwardToServiceAsync(ioc, req, socketPath, [&out](http::response<http::string_body> res) { out = std::move(res); });
        ioc.run();
        return out;
    }

    // ── Router ───────────────────────────────────────────────────────────────
    // Shared by both GatewaySession and GatewayTlsSession - routing doesn't depend
    // on the underlying stream type.

    static void routeAsync(const http::request<http::string_body> &req, ServiceController &ctrl,
                           asio::io_context &ioc, RouteCompletion done) {

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
            if (const auto service = detectEuclidService(req, ctrl); !service.empty()) {
                const auto action = std::string(req["x-euclid-action"]);
                if (!isPublicAction(service, action)) {
                    const auto auth = Core::HttpActionServer::Authenticate(req);
                    if (!auth.subject.has_value()) {
                        done(Core::HttpActionServer::Unauthorized(req, auth));
                        return;
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
                            done(err(http::status::bad_request, "requested concurrency (" + std::to_string(desired) + ") exceeds configured max instances for service '" + service + "'"));
                            return;
                        }
                    }
                }

                // Whether this request is evidence that the pool has work to do.
                //
                // Two things disqualify it, and they catch different cases. An action that holds
                // its connection open waiting for something to arrive is not working for almost
                // all of that time, whoever sent it - so receive-messages and its kin never count.
                // And a caller may say that its request is housekeeping rather than demand, which
                // is the only way to tell apart two requests that are the same action: a queue
                // depth read by an operator is a question about the system, the same read by a
                // metric collector every fifteen seconds is the system talking to itself.
                //
                // Absent means real, so a client that says nothing is counted - the default is the
                // safe one, and only a caller that knows it is instrumentation opts out. Nothing
                // is trusted to it either: at worst a pool scales less eagerly, and the caller
                // that lied is the one that suffers for it. It is deliberately not part of the
                // RFC 9421 covered components - adding a header there would invalidate every
                // signature produced by a client that predates it.
                static const std::set<std::string> kWaitingActions{"receive-messages", "receive-events", "subscribe-events"};
                const auto internal = std::string(req["x-euclid-internal"]);
                const bool countsAsLoad = !kWaitingActions.contains(action) && internal != "true";

                const auto handle = ctrl.acquireInstance(service, countsAsLoad);
                if (!handle) {
                    done(err(http::status::service_unavailable, "service '" + service + "' not registered or not running"));
                    return;
                }

                // Releases the autoscaler's active-request count when the exchange ends, however
                // it ends. Held by shared_ptr and captured into the completion because the forward
                // now outlives this function - and the name is stored by value rather than by
                // reference for the same reason: "service" is a local that is long gone by the
                // time the module answers.
                struct ReleaseGuard {
                    ServiceController &ctrl;
                    std::string name;
                    pid_t pid;
                    bool countsAsLoad;
                    ~ReleaseGuard() { ctrl.releaseInstance(name, pid, countsAsLoad); }
                };
                auto guard = std::make_shared<ReleaseGuard>(ctrl, service, handle->pid, countsAsLoad);

                forwardToServiceAsync(ioc, req, handle->socketPath,
                                      [done, guard](http::response<http::string_body> res) mutable {
                                          done(std::move(res));
                                      });
                return;
            }

            done(err(http::status::not_found, "not found"));
        } catch (const std::exception &ex) {
            log_error << "routeAsync() threw, error: " << ex.what();
            done(err(http::status::internal_server_error, ex.what()));
        } catch (...) {
            log_error << "routeAsync() threw a non-std::exception";
            done(err(http::status::internal_server_error, "internal server error"));
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

            // The deadline armed in doRead() was there to bound the wait for a request, but it
            // keeps running once one arrives, so until here it also had to cover the module
            // producing the answer. A long poll - receive-messages and receive-events deliberately
            // wait up to twenty seconds - could spend what was left of it and have this connection
            // closed under it while the module was still working, which the client sees as an EOF
            // while it waits for response headers rather than as any kind of answer. Re-armed here
            // so waiting for a request and doing the work behind it are budgeted separately, and
            // deliberately more generous than BackendTimeout(): that timer is the one meant to end
            // a slow exchange, and it can only do so if this one has not already killed the
            // connection its answer would travel back over.
            _stream.expires_after(BackendTimeout() + std::chrono::seconds(5));

            // The response arrives through the completion rather than being returned: while the
            // module is being waited on, this worker thread is free for other connections. self
            // is what keeps the parser - and so req, which the forward is still reading from -
            // alive until the answer comes back.
            routeAsync(req, _ctrl, _ioc, [self = shared_from_this()](http::response<http::string_body> res) {
                self->writeResponse(std::move(res));
            });
        }

        void writeResponse(http::response<http::string_body> res) {
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

            // The deadline armed in doRead() was there to bound the wait for a request, but it
            // keeps running once one arrives, so until here it also had to cover the module
            // producing the answer. A long poll - receive-messages and receive-events deliberately
            // wait up to twenty seconds - could spend what was left of it and have this connection
            // closed under it while the module was still working, which the client sees as an EOF
            // while it waits for response headers rather than as any kind of answer. Re-armed here
            // so waiting for a request and doing the work behind it are budgeted separately, and
            // deliberately more generous than BackendTimeout(): that timer is the one meant to end
            // a slow exchange, and it can only do so if this one has not already killed the
            // connection its answer would travel back over.
            beast::get_lowest_layer(_stream).expires_after(BackendTimeout() + std::chrono::seconds(5));

            // See the plain session: the answer comes back through the completion so this worker
            // is not held while the module produces it.
            routeAsync(req, _ctrl, _ioc, [self = shared_from_this()](http::response<http::string_body> res) {
                self->writeResponse(std::move(res));
            });
        }

        void writeResponse(http::response<http::string_body> res) {
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