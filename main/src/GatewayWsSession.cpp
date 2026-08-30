// Boost includes
#include <boost/asio/strand.hpp>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/WsFrame.h>
#include <euclid/manager/GatewayServer.h>
#include <euclid/manager/GatewayWsRegistry.h>
#include <euclid/manager/GatewayWsSession.h>

namespace Euclid::main {

    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace websocket = beast::websocket;
    namespace asio = boost::asio;

    namespace {

        std::size_t MaxMessageSize() {
            constexpr long kDefault = 1L * 1024 * 1024;
            return static_cast<std::size_t>(Core::Configuration::instance().getOr<long>("euclid.gateway.websocket.max-message-size", kDefault));
        }

        std::chrono::seconds IdleTimeout() {
            constexpr long kDefault = 300;
            return std::chrono::seconds(Core::Configuration::instance().getOr<long>("euclid.gateway.websocket.idle-timeout-seconds", kDefault));
        }

        template<class Stream>
        void ConfigureWs(websocket::stream<Stream> &ws) {
            auto opt = websocket::stream_base::timeout::suggested(beast::role_type::server);
            opt.idle_timeout = IdleTimeout();
            ws.set_option(opt);
            ws.read_message_max(MaxMessageSize());
        }

        // Shared by GatewayWsSession::handleRequestFrame() and GatewayWsTlsSession's - parses one
        // inbound frame and, if it's a well-formed request, dispatches the backend UDS round-trip
        // onto ioc so it runs independently of the read loop and of any other in-flight request
        // on the same connection (see GatewayWsSession.h's class doc comment). self is kept alive
        // for the duration via the captured shared_ptr.
        template<class SessionPtr>
        void DispatchFrame(SessionPtr self, boost::asio::io_context &ioc, ServiceController &ctrl, const std::string &authorization,
                            const std::string &accountId, const std::string &region, const std::string &ns, const std::string &text) {

            std::string error;
            auto parsed = Core::WsFrame::ParseRequest(text, error);
            if (!parsed.has_value()) {
                self->PostFrame(Core::WsFrame::BuildErrorFrame("", 400, error));
                return;
            }

            asio::post(ioc, [self, &ctrl, authorization, accountId, region, ns, request = std::move(*parsed)]() mutable {
                const auto id = request.id;
                const auto target = request.target;
                const auto httpReq = Core::WsFrame::ToHttpRequest(request, authorization, accountId, region, ns);

                const auto handle = ctrl.acquireInstance(target);
                if (!handle) {
                    self->PostFrame(Core::WsFrame::BuildErrorFrame(id, 503, "service '" + target + "' not registered or not running"));
                    return;
                }

                // Mirrors GatewayServer.cpp route()'s identical guard - guarantees the
                // autoscaler's active-request count is released on every exit path below.
                struct ReleaseGuard {
                    ServiceController &ctrl;
                    const std::string &name;
                    pid_t pid;
                    ~ReleaseGuard() { ctrl.releaseInstance(name, pid); }
                } guard{.ctrl = ctrl, .name = target, .pid = handle->pid};

                const auto res = forwardToService(httpReq, handle->socketPath);
                self->PostFrame(Core::WsFrame::BuildResponseFrame(id, res));
            });
        }

    }// namespace

    // ── GatewayWsSession ─────────────────────────────────────────────────────

    GatewayWsSession::GatewayWsSession(beast::tcp_stream stream, asio::io_context &ioc, ServiceController &ctrl,
                                        std::string authorization, std::string accountId, std::string region, std::string ns)
        : _ws(std::move(stream)), _ioc(ioc), _ctrl(ctrl), _authorization(std::move(authorization)), _accountId(std::move(accountId)), _region(std::move(region)), _namespace(std::move(ns)) {}

    void GatewayWsSession::run(http::request<http::string_body> req) {
        ConfigureWs(_ws);
        _ws.async_accept(req, [self = shared_from_this()](const beast::error_code &ec) { self->onAccept(ec); });
    }

    void GatewayWsSession::onAccept(const beast::error_code &ec) {
        if (ec) {
            log_warning << "GatewayWsSession: handshake failed, error: " << ec.message();
            return;
        }
        GatewayWsRegistry::instance().Register(_accountId, _region, weak_from_this());
        doRead();
    }

    void GatewayWsSession::doRead() {
        _ws.async_read(_buffer, [self = shared_from_this()](const beast::error_code &ec, const std::size_t bytes) { self->onRead(ec, bytes); });
    }

    void GatewayWsSession::onRead(const beast::error_code &ec, std::size_t) {
        if (ec) return;// EOF, timeout, etc. - session destructs once every outstanding handler drops its shared_ptr

        if (_ws.got_text()) handleRequestFrame(beast::buffers_to_string(_buffer.data()));
        _buffer.consume(_buffer.size());
        doRead();
    }

    void GatewayWsSession::handleRequestFrame(const std::string &text) {
        DispatchFrame(shared_from_this(), _ioc, _ctrl, _authorization, _accountId, _region, _namespace, text);
    }

    void GatewayWsSession::PostFrame(std::string frame) {
        asio::post(_ws.get_executor(), [self = shared_from_this(), frame = std::move(frame)]() mutable {
            self->_writeQueue.push_back(std::move(frame));
            if (self->_writeQueue.size() == 1) self->doWrite();
        });
    }

    void GatewayWsSession::doWrite() {
        _ws.text(true);
        _ws.async_write(asio::buffer(_writeQueue.front()), [self = shared_from_this()](const beast::error_code &ec, std::size_t) {
            if (ec) return;
            self->_writeQueue.pop_front();
            if (!self->_writeQueue.empty()) self->doWrite();
        });
    }

    // ── GatewayWsTlsSession ──────────────────────────────────────────────────

    GatewayWsTlsSession::GatewayWsTlsSession(beast::ssl_stream<beast::tcp_stream> stream, asio::io_context &ioc, ServiceController &ctrl,
                                              std::string authorization, std::string accountId, std::string region, std::string ns)
        : _ws(std::move(stream)), _ioc(ioc), _ctrl(ctrl), _authorization(std::move(authorization)), _accountId(std::move(accountId)), _region(std::move(region)), _namespace(std::move(ns)) {}

    void GatewayWsTlsSession::run(http::request<http::string_body> req) {
        ConfigureWs(_ws);
        _ws.async_accept(req, [self = shared_from_this()](const beast::error_code &ec) { self->onAccept(ec); });
    }

    void GatewayWsTlsSession::onAccept(const beast::error_code &ec) {
        if (ec) {
            log_warning << "GatewayWsTlsSession: handshake failed, error: " << ec.message();
            return;
        }
        GatewayWsRegistry::instance().Register(_accountId, _region, weak_from_this());
        doRead();
    }

    void GatewayWsTlsSession::doRead() {
        _ws.async_read(_buffer, [self = shared_from_this()](const beast::error_code &ec, const std::size_t bytes) { self->onRead(ec, bytes); });
    }

    void GatewayWsTlsSession::onRead(const beast::error_code &ec, std::size_t) {
        if (ec) return;

        if (_ws.got_text()) handleRequestFrame(beast::buffers_to_string(_buffer.data()));
        _buffer.consume(_buffer.size());
        doRead();
    }

    void GatewayWsTlsSession::handleRequestFrame(const std::string &text) {
        DispatchFrame(shared_from_this(), _ioc, _ctrl, _authorization, _accountId, _region, _namespace, text);
    }

    void GatewayWsTlsSession::PostFrame(std::string frame) {
        asio::post(_ws.get_executor(), [self = shared_from_this(), frame = std::move(frame)]() mutable {
            self->_writeQueue.push_back(std::move(frame));
            if (self->_writeQueue.size() == 1) self->doWrite();
        });
    }

    void GatewayWsTlsSession::doWrite() {
        _ws.text(true);
        _ws.async_write(asio::buffer(_writeQueue.front()), [self = shared_from_this()](const beast::error_code &ec, std::size_t) {
            if (ec) return;
            self->_writeQueue.pop_front();
            if (!self->_writeQueue.empty()) self->doWrite();
        });
    }

}// namespace Euclid::main
