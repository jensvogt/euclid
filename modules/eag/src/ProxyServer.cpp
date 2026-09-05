//
// Created by vogje01 on 9/5/26.
//

// Boost includes
#include <boost/asio/connect.hpp>
#include <boost/beast/core.hpp>

// Euclid includes
#include <ProxyServer.h>
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/LogStream.h>
#include <euclid/database/RepositoryFactory.h>

namespace Euclid::EAG {

    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = boost::beast::http;
    using tcp = boost::asio::ip::tcp;

    namespace {

        // How long a backend has to answer before the caller is told it did not.
        constexpr auto kBackendTimeout = std::chrono::seconds(60);

        // The request's path, without its query string - the route table matches on paths.
        std::string pathOf(const std::string &target) {
            const auto question = target.find('?');
            return question == std::string::npos ? target : target.substr(0, question);
        }

        http::response<http::string_body> errorResponse(const http::request<http::string_body> &req,
                                                        const http::status status, const std::string_view message) {
            http::response<http::string_body> res{status, req.version()};
            res.set(http::field::content_type, "application/json");
            res.keep_alive(req.keep_alive());
            res.body() = R"({"error":")" + std::string(message) + R"("})";
            res.prepare_payload();
            return res;
        }

    }// namespace

    ProxyServer::ProxyServer(const unsigned short port, const int threads, const long refreshSeconds)
        : _port(port), _threads(std::max(1, threads)), _refreshInterval(std::max(1L, refreshSeconds)),
          _acceptor(_ioc) {

        const tcp::endpoint endpoint{tcp::v4(), _port};
        _acceptor.open(endpoint.protocol());
        _acceptor.set_option(asio::socket_base::reuse_address(true));
        _acceptor.bind(endpoint);
        _acceptor.listen(asio::socket_base::max_listen_connections);
    }

    ProxyServer::~ProxyServer() {
        stop();
    }

    void ProxyServer::start() {

        if (_running.exchange(true)) return;

        // Read once before the first request rather than waiting for the first tick: a gateway
        // that answers "no route" for its first few seconds is indistinguishable from one that is
        // misconfigured.
        refresh();

        accept();

        for (int i = 0; i < _threads; ++i) {
            _workers.emplace_back([this] { _ioc.run(); });
        }

        _refreshThread = std::thread([this] {
            while (_running.load()) {
                std::this_thread::sleep_for(_refreshInterval);
                if (!_running.load()) break;
                refresh();
            }
        });

        log_info << "API gateway listening, port: " << _port << ", threads: " << _threads
                 << ", routes: " << _routes.size();
    }

    void ProxyServer::stop() {

        if (!_running.exchange(false)) return;

        boost::system::error_code ec;
        _acceptor.close(ec);
        _ioc.stop();

        if (_refreshThread.joinable()) _refreshThread.join();
        for (auto &worker: _workers) {
            if (worker.joinable()) worker.join();
        }
        _workers.clear();
        log_info << "API gateway stopped";
    }

    void ProxyServer::refresh() {

        _routes.refresh();

        // Only the applications the routes actually name, taken from the table that was just
        // refreshed, so the two cannot disagree about which applications matter - and so a failed
        // route read leaves the backends of the routes still being served alone.
        _backends.refresh(_routes.applicationIds());
    }

    void ProxyServer::accept() {

        _acceptor.async_accept([this](const boost::system::error_code &ec, tcp::socket socket) {
            if (ec) {
                // Expected once, when stop() closes the acceptor to unblock this.
                if (_running.load()) log_warning << "API gateway accept failed: " << ec.message();
                return;
            }
            serve(std::move(socket));
            if (_running.load()) accept();
        });
    }

    void ProxyServer::serve(tcp::socket socket) {

        // One exchange per connection for now: read a request, answer it, close. Keep-alive and
        // pipelining are what a busy gateway wants, and are the natural next step once this is
        // carrying real traffic.
        auto stream = std::make_shared<beast::tcp_stream>(std::move(socket));
        auto buffer = std::make_shared<beast::flat_buffer>();
        auto request = std::make_shared<http::request<http::string_body> >();

        stream->expires_after(kBackendTimeout);
        http::async_read(*stream, *buffer, *request,
                         [this, stream, buffer, request](const beast::error_code &ec, std::size_t) {
                             if (ec) {
                                 if (ec != http::error::end_of_stream) log_debug << "API gateway read failed: " << ec.message();
                                 return;
                             }
                             route(stream, request);
                         });
    }


    void ProxyServer::respond(const std::shared_ptr<beast::tcp_stream> &stream,
                              const std::shared_ptr<http::response<http::string_body> > &response) {

        http::async_write(*stream, *response, [stream, response](const beast::error_code &ec, std::size_t) {
            if (ec) log_debug << "API gateway write failed: " << ec.message();
            beast::error_code ignored;
            stream->socket().shutdown(tcp::socket::shutdown_send, ignored);
        });
    }

    void ProxyServer::route(const std::shared_ptr<beast::tcp_stream> &stream,
                            const std::shared_ptr<http::request<http::string_body> > &request) {

        const auto path = pathOf(std::string(request->target()));

        const auto match = _routes.match(path);
        if (!match.has_value()) {
            log_debug << "No route for " << path;
            respond(stream, std::make_shared<http::response<http::string_body> >(
                                    errorResponse(*request, http::status::not_found, "no route for this path")));
            return;
        }

        // Checked here, before a backend is chosen, so an unauthenticated caller never causes a
        // connection to an application at all.
        if (match->authentication == Database::Entity::EAG::RouteAuthentication::EUCLID) {
            if (const auto auth = Core::HttpActionServer::Authenticate(*request); !auth.subject.has_value()) {
                log_debug << "Unauthenticated request for " << path << ", route: " << match->routeId;
                respond(stream, std::make_shared<http::response<http::string_body> >(
                                        errorResponse(*request, http::status::unauthorized,
                                                      auth.denialReason.empty() ? "authentication required" : auth.denialReason)));
                return;
            }
        }

        const auto port = _backends.next(match->applicationId);
        if (!port.has_value()) {
            // The route is configured and the application is simply not there: scaled to zero,
            // still starting, or never given a port. Said as 503 rather than 404, because the
            // resource exists and the caller may reasonably try again.
            log_warning << "No running instance for application " << match->applicationId << ", route: " << match->routeId;
            respond(stream, std::make_shared<http::response<http::string_body> >(
                                    errorResponse(*request, http::status::service_unavailable,
                                                  "no running instance of application '" + match->applicationId + "'")));
            return;
        }

        proxyTo(stream, request, *port, match->routeId);
    }

    void ProxyServer::proxyTo(const std::shared_ptr<beast::tcp_stream> &stream,
                              const std::shared_ptr<http::request<http::string_body> > &request,
                              const int port, const std::string &routeId) {

        // The application is told who really called and over what, because after this it cannot
        // tell: every request it sees arrives from this process, on the loopback interface.
        auto forwarded = std::make_shared<http::request<http::string_body> >(*request);
        forwarded->set(http::field::host, "127.0.0.1:" + std::to_string(port));
        forwarded->set("X-Forwarded-Proto", "http");
        beast::error_code peerEc;
        if (const auto peer = stream->socket().remote_endpoint(peerEc); !peerEc) {
            forwarded->set("X-Forwarded-For", peer.address().to_string());
        }
        forwarded->prepare_payload();

        struct Exchange {
            explicit Exchange(asio::io_context &ioc) : backend(ioc) {}
            beast::tcp_stream backend;
            beast::flat_buffer buffer;
            // Buffered whole, both ways. That is the right shape for the REST calls this carries
            // today and the wrong one for a large download, which will want the body streamed
            // through rather than held here - the change belongs in this type and the two async
            // reads that fill it.
            http::response<http::string_body> response;
        };
        auto exchange = std::make_shared<Exchange>(_ioc);
        exchange->backend.expires_after(kBackendTimeout);

        const tcp::endpoint endpoint{asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(port)};
        exchange->backend.async_connect(endpoint, [this, exchange, stream, request, forwarded, port, routeId](const beast::error_code &ec) {
            if (ec) {
                log_warning << "Could not reach backend on port " << port << ", route: " << routeId << ", error: " << ec.message();
                respond(stream, std::make_shared<http::response<http::string_body> >(
                                        errorResponse(*request, http::status::bad_gateway, ec.message())));
                return;
            }
            http::async_write(exchange->backend, *forwarded, [this, exchange, stream, request, forwarded, port, routeId](const beast::error_code &writeEc, std::size_t) {
                if (writeEc) {
                    respond(stream, std::make_shared<http::response<http::string_body> >(
                                            errorResponse(*request, http::status::bad_gateway, writeEc.message())));
                    return;
                }
                http::async_read(exchange->backend, exchange->buffer, exchange->response,
                                 [this, exchange, stream, request, port, routeId](const beast::error_code &readEc, std::size_t) {
                                     if (readEc) {
                                         log_warning << "Backend on port " << port << " did not answer, route: " << routeId << ", error: " << readEc.message();
                                         respond(stream, std::make_shared<http::response<http::string_body> >(
                                                                 errorResponse(*request, http::status::bad_gateway, readEc.message())));
                                         return;
                                     }
                                     auto response = std::make_shared<http::response<http::string_body> >(std::move(exchange->response));
                                     response->keep_alive(false);
                                     response->prepare_payload();
                                     respond(stream, response);
                                 });
            });
        });
    }

}// namespace Euclid::EAG
