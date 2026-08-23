// C++ includes
#include <csignal>
#include <optional>
#include <unistd.h>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/UnixSocketServer.h>

namespace Euclid::Core {

    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace asio = boost::asio;
    namespace local = asio::local;

    UnixSocketServer *UnixSocketServer::s_instance = nullptr;

    namespace {
        // Boost.Beast's parser defaults to a 1MB body limit when none is set explicitly, which a
        // single storage upload-part can silently exceed (default part size alone is 5MB) - the
        // parser then fails the read and the session tears down with no response, surfacing to
        // the client as a bare "connection reset". Reuses the gateway's max-body config (already
        // shipped in dist/etc/euclid*.json) since requests reaching here already passed through
        // that same limit at the gateway hop.
        std::uint64_t MaxBodySize() {
            constexpr long kDefaultMaxBodySize = 512L * 1024 * 1024;
            return static_cast<std::uint64_t>(Configuration::instance().getOr<long>("euclid.gateway.http.max-body", kDefaultMaxBodySize));
        }
    }// namespace

    // ── Session ──────────────────────────────────────────────────────────────
    // Handles one Unix-socket connection: reads requests in a loop, dispatches
    // them to the owning server, writes responses.

    class UnixSocketServer::Session : public std::enable_shared_from_this<Session> {
    public:

        Session(local::stream_protocol::socket sock, UnixSocketServer &server) : _stream(std::move(sock)), _server(server) {}

        void run() { doRead(); }

    private:

        void doRead() {
            _parser.emplace();
            _parser->body_limit(MaxBodySize());
            http::async_read(_stream, _buf, *_parser, [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) return;

                auto &req = self->_parser->get();

                // A handler-level exception (e.g. malformed request body) must not take
                // down the whole service - report it as a 500 instead of letting it
                // escape the io_context worker thread and terminate the process.
                try {
                    self->doWrite(self->_server.Dispatch(req));
                } catch (const std::exception &e) {
                    log_error << self->_server._serviceName << " request handler threw, error: " << e.what();
                    http::response<http::string_body> res{http::status::internal_server_error, req.version()};
                    res.set(http::field::content_type, "application/json");
                    res.body() = R"({"error":"internal server error"})";
                    res.prepare_payload();
                    self->doWrite(std::move(res));
                }
            });
        }

        void doWrite(http::response<http::string_body> res) {
            const bool keepAlive = res.keep_alive();
            auto sp = std::make_shared<http::response<http::string_body> >(std::move(res));
            http::async_write(_stream, *sp, [self = shared_from_this(), sp, keepAlive](beast::error_code ec, std::size_t) {
                if (!ec && keepAlive) self->doRead();
            });
        }

        // beast generic stream wrapper for a local socket
        beast::basic_stream<local::stream_protocol> _stream;
        beast::flat_buffer _buf;
        std::optional<http::request_parser<http::string_body> > _parser;
        UnixSocketServer &_server;
    };

    // ── UnixSocketServer ─────────────────────────────────────────────────────

    UnixSocketServer::UnixSocketServer(std::string serviceName, std::string socketPath, const int threads)
        : _serviceName(std::move(serviceName)), _socketPath(std::move(socketPath)), _ioc(threads), _acceptor(_ioc), _threads(threads) {

        unlink(_socketPath.c_str());

        const local::stream_protocol::endpoint ep(_socketPath);
        _acceptor.open(ep.protocol());
        _acceptor.bind(ep);
        _acceptor.listen(asio::socket_base::max_listen_connections);

        log_info << _serviceName << " service listening on " << _socketPath;
    }

    void UnixSocketServer::start() {
        doAccept();
        _workers.reserve(_threads);
        for (int i = 0; i < _threads; ++i) _workers.emplace_back([this] { _ioc.run(); });
    }

    void UnixSocketServer::stop() {
        _ioc.stop();
        for (auto &t: _workers) if (t.joinable()) t.join();
        ::unlink(_socketPath.c_str());
        log_info << _serviceName << " service stopped";
    }

    void UnixSocketServer::doAccept() {
        _acceptor.async_accept([this](const beast::error_code &ec, local::stream_protocol::socket sock) {
            if (!ec) std::make_shared<Session>(std::move(sock), *this)->run();
            doAccept();
        });
    }

    void UnixSocketServer::onSignal(int) {
        if (s_instance) s_instance->stop();
    }

    int UnixSocketServer::RunUntilSignal() {
        s_instance = this;
        std::signal(SIGTERM, onSignal);
        std::signal(SIGINT, onSignal);

        start();

        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGTERM);
        sigaddset(&mask, SIGINT);
        int sig = 0;
        sigwait(&mask, &sig);

        stop();
        return 0;
    }

}// namespace Euclid::Core
