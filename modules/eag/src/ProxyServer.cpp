//
// Created by vogje01 on 9/5/26.
//

// Boost includes
#include <boost/asio/connect.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>

// Euclid includes
#include <ProxyServer.h>
#include <euclid/core/Configuration.h>
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

    ProxyServer::ProxyServer(std::vector<Listener> listeners, const int threads, const long refreshSeconds,
                             const long basicAuthCacheSeconds, const unsigned short euclidGatewayPort,
                             const bool euclidGatewayTls, const std::string &euclidGatewayCert)
        : _listeners(std::move(listeners)), _threads(std::max(1, threads)), _refreshInterval(std::max(1L, refreshSeconds)),
          _basicAuth(basicAuthCacheSeconds), _euclidGatewayPort(euclidGatewayPort),
          _euclidGatewayTls(euclidGatewayTls), _euclidGatewayCtx(asio::ssl::context::tlsv12_client),
          _region(Core::Configuration::instance().getOr<std::string>("euclid.region", "")) {

        if (_euclidGatewayTls) {
            _euclidGatewayCtx.set_default_verify_paths();

            // The gateway's own certificate as the trust anchor. It is self-signed in every
            // installation that has not been given a real one, and a self-signed certificate is
            // its own issuer - so loading it here is what makes verify_peer able to succeed at
            // all. Hostname checking is deliberately not added on top: this connects to 127.0.0.1
            // and a certificate issued for a host name would never match that.
            if (!euclidGatewayCert.empty()) {
                boost::system::error_code ec;
                _euclidGatewayCtx.load_verify_file(euclidGatewayCert, ec);
                if (ec) {
                    log_warning << "Could not load the euclid gateway certificate, module routes may fail, file: "
                                << euclidGatewayCert << ", error: " << ec.message();
                }
            }
            _euclidGatewayCtx.set_verify_mode(asio::ssl::verify_peer);
        }

        // Bound in the constructor, so a port already in use is a failure the module reports at
        // start-up rather than one that shows as a listener nobody can reach.
        for (const auto &listener: _listeners) {
            const tcp::endpoint endpoint{tcp::v4(), listener.port};
            auto &acceptor = _acceptors.emplace_back(_ioc);
            acceptor.open(endpoint.protocol());
            acceptor.set_option(asio::socket_base::reuse_address(true));
            acceptor.bind(endpoint);
            acceptor.listen(asio::socket_base::max_listen_connections);
        }
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

        for (std::size_t i = 0; i < _acceptors.size(); ++i) accept(i);

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

        std::string listening;
        for (const auto &listener: _listeners) {
            if (!listening.empty()) listening += ", ";
            listening += std::to_string(listener.port);
            if (!listener.nameSpace.empty()) listening += " (" + listener.nameSpace + ")";
        }
        log_info << "API gateway listening on " << listening << ", threads: " << _threads
                 << ", routes: " << _routes.size();
    }

    void ProxyServer::stop() {

        if (!_running.exchange(false)) return;

        boost::system::error_code ec;
        for (auto &acceptor: _acceptors) acceptor.close(ec);
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

    void ProxyServer::accept(const std::size_t index) {

        _acceptors[index].async_accept([this, index](const boost::system::error_code &ec, tcp::socket socket) {
            if (ec) {
                // Expected once per listener, when stop() closes the acceptors to unblock these.
                if (_running.load()) log_warning << "API gateway accept failed on port " << _listeners[index].port << ": " << ec.message();
                return;
            }
            // The namespace comes from the port the connection arrived on, which is the whole
            // point of having several: it says which environment was meant without the caller
            // having to put it in the URL.
            serve(std::move(socket), _listeners[index].nameSpace);
            if (_running.load()) accept(index);
        });
    }

    void ProxyServer::serve(tcp::socket socket, const std::string &nameSpace) {

        // One exchange per connection for now: read a request, answer it, close. Keep-alive and
        // pipelining are what a busy gateway wants, and are the natural next step once this is
        // carrying real traffic.
        auto stream = std::make_shared<beast::tcp_stream>(std::move(socket));
        auto buffer = std::make_shared<beast::flat_buffer>();
        auto request = std::make_shared<http::request<http::string_body> >();

        stream->expires_after(kBackendTimeout);
        http::async_read(*stream, *buffer, *request,
                         [this, stream, buffer, request, nameSpace](const beast::error_code &ec, std::size_t) {
                             if (ec) {
                                 if (ec != http::error::end_of_stream) log_debug << "API gateway read failed: " << ec.message();
                                 return;
                             }
                             route(nameSpace, stream, request);
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

    void ProxyServer::route(const std::string &nameSpace,
                            const std::shared_ptr<beast::tcp_stream> &stream,
                            const std::shared_ptr<http::request<http::string_body> > &request) {

        const auto path = pathOf(std::string(request->target()));
        const auto method = std::string(request->method_string());


        const auto found = _routes.match(path, method, nameSpace);
        if (!found.route.has_value()) {

            // A path that exists but does not take this method is 405, not 404 - the two say
            // quite different things to whoever is holding the URL, and the Allow header tells
            // them which methods would have worked instead of leaving them to guess.
            if (!found.allowed.empty()) {
                std::string allow;
                for (const auto &allowed: found.allowed) {
                    if (!allow.empty()) allow += ", ";
                    allow += allowed;
                }
                log_debug << "Method " << method << " not routed for " << path << ", allowed: " << allow;

                auto response = std::make_shared<http::response<http::string_body> >(
                        errorResponse(*request, http::status::method_not_allowed, "method not allowed for this path"));
                response->set(http::field::allow, allow);
                respond(stream, response);
                return;
            }

            log_debug << "No route for " << path;
            respond(stream, std::make_shared<http::response<http::string_body> >(
                                    errorResponse(*request, http::status::not_found, "no route for this path")));
            return;
        }
        const auto &match = found.route;

        // The scope euclid works in, taken from the route, for a caller that does not know euclid
        // exists. CheckScope() refuses a request naming no region once euclid.region is set -
        // right between modules, where every client sends it, and not something a browser could
        // know to do. Translating an outside request into a euclid one is what this gateway is
        // for, so it supplies what the caller could not have known to send.
        //
        // From the route rather than from configuration, because a namespace is a property of
        // what is published and not of the installation: two routes on one gateway may belong to
        // different namespaces, and only the route says which.
        //
        // Only when absent. A caller that names a region or a namespace is asking for that one,
        // and being told it is not permitted is a better answer than being moved somewhere else
        // without being told.
        if ((*request)["x-euclid-region"].empty()) {
            if (const auto &region = !match->region.empty() ? match->region : _region; !region.empty()) {
                request->set("x-euclid-region", region);
            }
        }
        if ((*request)["x-euclid-namespace"].empty()) {
            if (const auto &ns = !match->nameSpace.empty() ? match->nameSpace : nameSpace; !ns.empty()) {
                request->set("x-euclid-namespace", ns);
            }
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
        } else if (match->authentication == Database::Entity::EAG::RouteAuthentication::BASIC) {
            if (!_basicAuth.Verify(std::string((*request)[http::field::authorization])).has_value()) {
                log_debug << "Basic authentication required for " << path << ", route: " << match->routeId;

                // The header is the whole point of Basic over a bearer token: without it a browser
                // shows the 401 body, with it the browser asks the person for a username and
                // password and tries again. The realm is what it shows them.
                auto response = std::make_shared<http::response<http::string_body> >(
                        errorResponse(*request, http::status::unauthorized, "authentication required"));
                response->set(http::field::www_authenticate, R"(Basic realm="euclid", charset="UTF-8")");
                respond(stream, response);
                return;
            }
        }

        // A route naming a module goes to euclid's own gateway rather than to an application:
        // modules listen on Unix domain sockets and have no address anything outside the host
        // could reach, and the gateway is what turns a target and an action into a call on one.
        if (!match->moduleTarget.empty()) {
            if (_euclidGatewayTls) {
                proxyToTls(stream, request, _euclidGatewayPort, match->routeId, match->moduleTarget, match->moduleAction);
            } else {
                proxyTo(stream, request, _euclidGatewayPort, match->routeId, match->moduleTarget, match->moduleAction);
            }
            return;
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

        proxyTo(stream, request, *port, match->routeId, {}, {});
    }

    // The request as the backend should see it: the caller's own, with the hop rewritten and -
    // for a module route - the target and action the route names. Shared so the plain and TLS
    // paths cannot drift apart about what they forward.
    static std::shared_ptr<http::request<http::string_body> > forwardedRequest(
            const std::shared_ptr<beast::tcp_stream> &stream,
            const std::shared_ptr<http::request<http::string_body> > &request,
            const int port, const std::string &euclidTarget, const std::string &euclidAction) {

        auto forwarded = std::make_shared<http::request<http::string_body> >(*request);
        forwarded->set(http::field::host, "127.0.0.1:" + std::to_string(port));
        forwarded->set("X-Forwarded-Proto", "http");

        beast::error_code peerEc;
        if (const auto peer = stream->socket().remote_endpoint(peerEc); !peerEc) {
            forwarded->set("X-Forwarded-For", peer.address().to_string());
        }

        // Which module and which action, for a route that names one. Set here rather than expected
        // from the caller: the route decides what it publishes, and a client that could choose its
        // own action would have every action of that module rather than the one written down.
        if (!euclidTarget.empty()) {
            forwarded->set("x-euclid-target", euclidTarget);
            forwarded->set("x-euclid-action", euclidAction);
        }

        forwarded->prepare_payload();
        return forwarded;
    }

    void ProxyServer::proxyToTls(const std::shared_ptr<beast::tcp_stream> &stream,
                                 const std::shared_ptr<http::request<http::string_body> > &request,
                                 const int port, const std::string &routeId,
                                 const std::string &euclidTarget, const std::string &euclidAction) {

        const auto forwarded = forwardedRequest(stream, request, port, euclidTarget, euclidAction);

        struct Exchange {
            Exchange(asio::io_context &ioc, asio::ssl::context &ctx) : backend(ioc, ctx) {}
            beast::ssl_stream<beast::tcp_stream> backend;
            beast::flat_buffer buffer;
            http::response<http::string_body> response;
        };
        auto exchange = std::make_shared<Exchange>(_ioc, _euclidGatewayCtx);

        // SNI. The gateway does not select a certificate by name, but some OpenSSL builds refuse
        // the handshake outright when the extension is absent, and it costs nothing.
        if (!SSL_set_tlsext_host_name(exchange->backend.native_handle(), "localhost")) {
            log_warning << "Could not set the TLS server name, route: " << routeId;
        }
        beast::get_lowest_layer(exchange->backend).expires_after(kBackendTimeout);

        const tcp::endpoint endpoint{asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(port)};
        beast::get_lowest_layer(exchange->backend).async_connect(endpoint, [this, exchange, stream, request, forwarded, port, routeId](const beast::error_code &ec) {
            if (ec) {
                log_warning << "Could not reach the euclid gateway on port " << port << ", route: " << routeId << ", error: " << ec.message();
                respond(stream, std::make_shared<http::response<http::string_body> >(
                                        errorResponse(*request, http::status::bad_gateway, ec.message())));
                return;
            }
            exchange->backend.async_handshake(asio::ssl::stream_base::client, [this, exchange, stream, request, forwarded, port, routeId](const beast::error_code &shakeEc) {
                if (shakeEc) {
                    // Almost always the certificate: euclid.gateway.tls.cert-file could not be
                    // read, or the gateway was given one this process does not trust.
                    log_warning << "TLS handshake with the euclid gateway failed, route: " << routeId << ", error: " << shakeEc.message();
                    respond(stream, std::make_shared<http::response<http::string_body> >(
                                            errorResponse(*request, http::status::bad_gateway,
                                                          "TLS handshake with the euclid gateway failed: " + shakeEc.message())));
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
                                             log_warning << "The euclid gateway on port " << port << " did not answer, route: " << routeId << ", error: " << readEc.message();
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
        });
    }

    void ProxyServer::proxyTo(const std::shared_ptr<beast::tcp_stream> &stream,
                              const std::shared_ptr<http::request<http::string_body> > &request,
                              const int port, const std::string &routeId,
                              const std::string &euclidTarget, const std::string &euclidAction) {

        // The application is told who really called and over what, because after this it cannot
        // tell: every request it sees arrives from this process, on the loopback interface.
        const auto forwarded = forwardedRequest(stream, request, port, euclidTarget, euclidAction);

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
