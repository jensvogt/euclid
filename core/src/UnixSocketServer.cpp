// C++ includes
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <mutex>
#include <optional>
#if defined(_WIN32)
#include <windows.h>
#endif

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

        std::remove(_socketPath.c_str());

        const local::stream_protocol::endpoint ep(_socketPath);
        _acceptor.open(ep.protocol());
        _acceptor.bind(ep);
        _acceptor.listen(asio::socket_base::max_listen_connections);

        // The thread count is here because it is not otherwise visible anywhere: it decides how
        // many requests this instance can be in the middle of at once, and a module sized too
        // small does not fail - its callers just wait, which reads as the module being slow.
        log_info << _serviceName << " service listening on " << _socketPath << ", worker threads: " << _threads;
    }

    void UnixSocketServer::start() {
        doAccept();
        _workers.reserve(_threads);
        for (int i = 0; i < _threads; ++i) _workers.emplace_back([this] { _ioc.run(); });
    }

    void UnixSocketServer::stop() {
        _ioc.stop();
        for (auto &t: _workers) if (t.joinable()) t.join();
        std::remove(_socketPath.c_str());
        log_info << _serviceName << " service stopped";
    }

    void UnixSocketServer::doAccept() {
        _acceptor.async_accept([this](const beast::error_code &ec, local::stream_protocol::socket sock) {
            if (!ec) std::make_shared<Session>(std::move(sock), *this)->run();
            doAccept();
        });
    }

    namespace {
        std::mutex s_signalMutex;
        std::condition_variable s_signalCv;
        bool s_signalled = false;
    }// namespace

    void UnixSocketServer::onSignal(int) {
        {
            std::lock_guard lock(s_signalMutex);
            s_signalled = true;
        }
        s_signalCv.notify_all();
    }

#if defined(_WIN32)
    namespace {
        // Console control events aren't delivered through std::signal()'s SIGTERM/SIGINT on
        // Windows - a process can only be asked to shut down gracefully via
        // SetConsoleCtrlHandler. This is what ServiceController::stopInstance's
        // Platform::RequestGracefulStop() (CTRL_BREAK_EVENT) actually reaches on this side.
        // Sets the same s_signalMutex/s_signalCv/s_signalled state onSignal() does above
        // directly, rather than calling onSignal() (a private UnixSocketServer member a
        // free function can't reach).
        BOOL WINAPI consoleHandler(const DWORD ctrlType) {
            switch (ctrlType) {
                case CTRL_C_EVENT:
                case CTRL_BREAK_EVENT:
                case CTRL_CLOSE_EVENT:
                case CTRL_SHUTDOWN_EVENT: {
                    {
                        std::lock_guard lock(s_signalMutex);
                        s_signalled = true;
                    }
                    s_signalCv.notify_all();
                    return TRUE;
                }
                default:
                    return FALSE;
            }
        }
    }// namespace
#endif

    int UnixSocketServer::RunUntilSignal() {
        s_instance = this;
#if defined(_WIN32)
        SetConsoleCtrlHandler(consoleHandler, TRUE);
#else
        std::signal(SIGTERM, onSignal);
        std::signal(SIGINT, onSignal);
#endif

        start();

        std::unique_lock lock(s_signalMutex);
        s_signalCv.wait(lock, [] { return s_signalled; });

        stop();
        return 0;
    }

}// namespace Euclid::Core
