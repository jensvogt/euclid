// Boost includes
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/ssl.hpp>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/core/SigV4.h>
#include <euclid/manager/GatewayServer.h>

namespace Euclid::main {

    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace asio = boost::asio;
    using tcp = asio::ip::tcp;

    // ── AWS service detection ────────────────────────────────────────────────

    // Returns the lowercase AWS service name for a request, or empty string if
    // this is not an AWS service call.
    //
    // Detection order:
    //   1. x-euclid-target — "DynamoDB_20120810.PutItem" → "dynamodb"
    //   2. Authorization   — SigV4 credential scope: "<key>/<date>/<region>/<svc>/aws4_request"
    static std::string detectAwsService(const http::request<http::string_body> &req) {
        static const std::unordered_set<std::string> kModules{
            "access", "s3", "queues", "sns", "dynamodb", "kinesis", "lambda",
            "iam", "sts", "ec2", "secretsmanager", "ssm", "kms",
            "firehose", "events", "logs", "cognito-idp", "cognito-identity"
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
        static const std::unordered_set<std::string> kPublic{"access:login", "access:register"};
        return kPublic.contains(service + ":" + action);
    }

    // Proxies req over a Unix-domain socket at socketPath and returns the
    // backend's response.  Runs synchronously on the calling worker thread.
    static http::response<http::string_body> forwardToService(const http::request<http::string_body> &req, const std::string &socketPath) {
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

        try {
            asio::io_context ioc;
            local::stream_protocol::socket sock(ioc);
            sock.connect(local::stream_protocol::endpoint(socketPath));
            http::write(sock, req);
            beast::flat_buffer buf;
            http::response<http::string_body> res;
            http::read(sock, buf, res);
            return res;
        } catch (const std::exception &ex) {
            return errorResponse(http::status::bad_gateway, ex.what());
        }
    }

    // ── Router ───────────────────────────────────────────────────────────────
    // Shared by both GatewaySession and GatewayTlsSession - routing doesn't depend
    // on the underlying stream type.

    static http::response<http::string_body> route(const http::request<http::string_body> &req, ServiceController &ctrl) {

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

        // ── AWS service dispatch ─────────────────────────────────────────
        if (const auto service = detectAwsService(req); !service.empty()) {
            if (const auto action = std::string(req["x-euclid-action"]); !isPublicAction(service, action)) {
                if (const auto auth = Core::HttpActionServer::Authenticate(req); !auth.subject.has_value()) {
                    return Core::HttpActionServer::Unauthorized(req, auth);
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
    }

    // ── GatewaySession ───────────────────────────────────────────────────────
    // Handles one plaintext TCP connection: reads requests in a loop (keep-alive
    // aware), dispatches to the router, and writes responses.

    class GatewaySession : public std::enable_shared_from_this<GatewaySession> {
    public:
        GatewaySession(tcp::socket socket, ServiceController &ctrl) : _stream(std::move(socket)), _ctrl(ctrl) {
        }

        void run() { doRead(); }

    private:
        void doRead() {
            _req = {};
            _stream.expires_after(std::chrono::seconds(30));
            http::async_read(
                _stream, _buf, _req,
                [self = shared_from_this()](const beast::error_code &ec, std::size_t) {
                    if (!ec) self->handleRequest();
                    // on error (EOF, timeout, etc.) the session destructs naturally
                });
        }

        void handleRequest() {
            auto res = route(_req, _ctrl);
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

        beast::tcp_stream _stream;
        beast::flat_buffer _buf;
        http::request<http::string_body> _req;
        ServiceController &_ctrl;
    };

    // ── GatewayTlsSession ────────────────────────────────────────────────────
    // Same request loop as GatewaySession, but the connection is terminated with
    // TLS: a handshake runs before the first read, and the stream is an SSL layer
    // wrapped around the TCP stream.

    class GatewayTlsSession : public std::enable_shared_from_this<GatewayTlsSession> {
    public:
        GatewayTlsSession(tcp::socket socket, asio::ssl::context &sslCtx, ServiceController &ctrl)
            : _stream(std::move(socket), sslCtx), _ctrl(ctrl) {
        }

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
            _req = {};
            beast::get_lowest_layer(_stream).expires_after(std::chrono::seconds(30));
            http::async_read(
                _stream, _buf, _req,
                [self = shared_from_this()](const beast::error_code &ec, std::size_t) {
                    if (!ec) self->handleRequest();
                    // on error (EOF, timeout, etc.) the session destructs naturally
                });
        }

        void handleRequest() {
            auto res = route(_req, _ctrl);
            auto sp = std::make_shared<http::response<http::string_body> >(std::move(res));
            const bool keepAlive = sp->keep_alive();
            http::async_write(
                _stream, *sp,
                [self = shared_from_this(), sp, keepAlive](const beast::error_code &ec, std::size_t) {
                    if (!ec && keepAlive) self->doRead();
                    else {
                        beast::error_code ignored;
                        std::ignore = self->_stream.shutdown(ignored);
                    }
                });
        }

        beast::ssl_stream<beast::tcp_stream> _stream;
        beast::flat_buffer _buf;
        http::request<http::string_body> _req;
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
                if (_tlsEnabled) std::make_shared<GatewayTlsSession>(std::move(socket), _sslCtx, _ctrl)->run();
                else std::make_shared<GatewaySession>(std::move(socket), _ctrl)->run();
            }
            if (!_ioc.stopped()) doAccept();
        });
    }

} // namespace Euclid::main