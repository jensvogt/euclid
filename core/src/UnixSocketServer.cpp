// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <filesystem>
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

        // The directory first. bind() answers ENOENT - "No such file or directory" - when the
        // *parent* is missing, which reads as though the socket itself were expected to exist and
        // sends anybody debugging it looking for the wrong thing.
        //
        // It is routinely missing, and not only on a fresh container: sockets live under /var/run,
        // which is a tmpfs on Linux and is therefore empty again after every reboot. A directory
        // created by a package at install time does not survive, so creating it here - on the one
        // path every module and the gateway bind through - is what makes a socket path in the
        // configuration mean what it says without anything else having to prepare the ground.
        if (const auto parent = std::filesystem::path(_socketPath).parent_path(); !parent.empty()) {
            if (std::error_code ec; !std::filesystem::create_directories(parent, ec) && ec) {
                log_warning << "Could not create the socket directory, path: " << parent.string() << ", error: " << ec.message();
            }
        }

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

        // Set by SIGUSR1 rather than acted on in the handler: re-reading a configuration file
        // allocates, takes locks and logs, none of which is safe to do from a signal handler. The
        // wait below is woken and does the work on an ordinary thread.
        std::atomic<bool> s_reloadLogging{false};
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

        // Re-read the configuration and apply the log levels in it - see onLogReloadSignal().
        // Nothing else about the process changes, so this is safe to send to a module that is in
        // the middle of serving requests.
        std::signal(SIGUSR1, onLogReloadSignal);
#endif

        start();

        std::unique_lock lock(s_signalMutex);
        while (true) {
            s_signalCv.wait(lock, [] { return s_signalled || s_reloadLogging.load(); });
            if (s_signalled) break;

            // Outside the handler and outside the lock the handler runs under, since this reads a
            // file and logs what it found.
            s_reloadLogging.store(false);
            lock.unlock();
            reloadLogging();
            lock.lock();
        }

        stop();
        return 0;
    }

    void UnixSocketServer::onLogReloadSignal(int) {
        s_reloadLogging.store(true);
        s_signalCv.notify_all();
    }

    void UnixSocketServer::reloadLogging() {
        try {
            Configuration::instance().reload();
        } catch (const std::exception &e) {
            log_error << "Could not re-read the configuration, keeping the current log levels, error: " << e.what();
            return;
        }
        LogStream::ApplyConfiguration();
        log_info << "Log levels reloaded, level: " << LogStream::GetSeverity();
    }

}// namespace Euclid::Core
