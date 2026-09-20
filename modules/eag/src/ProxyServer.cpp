// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/5/26.
//

// C++ includes
#include <algorithm>
#include <cctype>

// Boost includes
#include <boost/asio/connect.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/json.hpp>

// Euclid includes
#include <ListenerCertificate.h>
#include <ModuleCall.h>
#include <ProxyServer.h>
#include <UploadTarget.h>
#include <euclid/core/Configuration.h>
#include <euclid/core/CryptoUtils.h>
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/HttpSignature.h>
#include <euclid/core/JwtUtils.h>
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

        // How much of a proxied request the gateway will hold. Beast's own default is 8 MB, which
        // this used to inherit by reading into a message rather than a parser - a limit nobody
        // chose, and one that is both too small for an application taking a file and too large to
        // be reached by a thousand callers at once. Shared with euclid's own gateway, since a
        // request that goes through this one to a module has to pass both.
        std::uint64_t maxProxyBody() {
            constexpr long kDefaultMaxBodySize = 512L * 1024 * 1024;
            return static_cast<std::uint64_t>(
                    Core::Configuration::instance().getOr<long>("euclid.gateway.http.max-body", kDefaultMaxBodySize));
        }

        // Templated on the body, because a refusal is often decided before the body has been read
        // and the request is then an http::request<http::empty_body> - a route that does not
        // exist, a method it does not take, a body larger than it accepts. Nothing here reads the
        // body, so there is nothing for the two instantiations to disagree about.
        template<class Body>
        http::response<http::string_body> errorResponse(const http::request<Body> &req,
                                                        const http::status status, const std::string_view message) {
            http::response<http::string_body> res{status, req.version()};
            res.set(http::field::content_type, "application/json");
            res.keep_alive(req.keep_alive());
            res.body() = R"({"error":")" + std::string(message) + R"("})";
            res.prepare_payload();
            return res;
        }

    }// namespace

    std::optional<Protocol> ProtocolFromString(std::string protocol) {
        std::ranges::transform(protocol, protocol.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (protocol.empty() || protocol == "http") return Protocol::HTTP;
        if (protocol == "https") return Protocol::HTTPS;
        return std::nullopt;
    }

    std::string ProtocolToString(const Protocol protocol) {
        return protocol == Protocol::HTTPS ? "https" : "http";
    }

    ProxyServer::ProxyServer(std::vector<Listener> listeners, const int threads, const long refreshSeconds,
                             const long basicAuthCacheSeconds, const unsigned short euclidGatewayPort,
                             const bool euclidGatewayTls, const std::string &euclidGatewayCert)
        : _listeners(std::move(listeners)), _threads(std::max(1, threads)), _refreshInterval(std::max(1L, refreshSeconds)),
          _basicAuth(basicAuthCacheSeconds), _euclidGatewayPort(euclidGatewayPort),
          _euclidGatewayTls(euclidGatewayTls),
          _region(Core::Configuration::instance().getOr<std::string>("euclid.region", "")),
          _euclidGatewayCtx(asio::ssl::context::tlsv12_client) {

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
        // start-up rather than one that shows as a listener nobody can reach. The certificate of
        // an HTTPS listener is loaded here for the same reason: a port that cannot terminate TLS
        // has nothing to offer a caller, and finding that out now beats finding it out one failed
        // handshake at a time.
        for (const auto &listener: _listeners) {
            const tcp::endpoint endpoint{tcp::v4(), listener.port};
            auto &acceptor = _acceptors.emplace_back(_ioc);
            acceptor.open(endpoint.protocol());
            acceptor.set_option(asio::socket_base::reuse_address(true));
            acceptor.bind(endpoint);
            acceptor.listen(asio::socket_base::max_listen_connections);

            _serverContexts.push_back(listener.protocol == Protocol::HTTPS
                                              ? LoadListenerCertificate(listener.certificate, listener.nameSpace)
                                              : nullptr);
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
            listening += ProtocolToString(listener.protocol) + ":" + std::to_string(listener.port);
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
        _backends.refresh(_routes.applications());
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
            serve(std::move(socket), index);
            if (_running.load()) accept(index);
        });
    }

    void ProxyServer::serve(tcp::socket socket, const std::size_t index) {

        // One exchange per connection for now: read a request, answer it, close. Keep-alive and
        // pipelining are what a busy gateway wants, and are the natural next step once this is
        // carrying real traffic.
        const auto &context = _serverContexts[index];
        auto stream = context ? std::make_shared<ClientStream>(std::move(socket), context)
                              : std::make_shared<ClientStream>(std::move(socket));
        auto buffer = std::make_shared<beast::flat_buffer>();

        // Headers first, body afterwards: what the body should be read into is decided by the
        // route, and the route is not known until the path and the method have arrived. See
        // dispatch().
        auto header = std::make_shared<ClientStream::HeaderReader>();

        // No limit on the header reader, which is not the same as no limit: Beast checks
        // body_limit against the declared Content-Length while it is still parsing headers, so a
        // reader left at its 8 MB default refuses a large upload before the route that permits it
        // has even been looked up. The limit belongs to whichever reader actually takes the body,
        // and dispatch() sets it there.
        header->body_limit(boost::none);

        const auto nameSpace = _listeners[index].nameSpace;

        stream->ExpiresAfter(kBackendTimeout);

        // Unconditional, because a plain connection is simply one whose handshake is already
        // done - see ClientStream::Handshake. What follows is then the same either way.
        stream->Handshake([this, stream, buffer, header, nameSpace, index](const beast::error_code &handshakeEc) {
            if (handshakeEc) {
                // Ordinary on a public port: a caller speaking plain HTTP to it, a client with no
                // protocol version in common, or a scanner. Debug rather than warning, or the log
                // becomes a record of everything on the internet that ever knocked.
                log_debug << "TLS handshake failed on port " << _listeners[index].port << ": " << handshakeEc.message();
                return;
            }
            stream->ExpiresAfter(kBackendTimeout);
            stream->AsyncReadHeader(*buffer, *header, [this, stream, buffer, header, nameSpace](const beast::error_code &ec) {
                if (ec) {
                    if (ec != http::error::end_of_stream) log_debug << "API gateway read failed: " << ec.message();
                    return;
                }
                dispatch(nameSpace, stream, buffer, header);
            });
        });
    }


    void ProxyServer::respond(const std::shared_ptr<ClientStream> &stream,
                              const std::shared_ptr<http::response<http::string_body> > &response) {

        stream->AsyncWrite(*response, [stream, response](const beast::error_code &ec) {
            if (ec) log_debug << "API gateway write failed: " << ec.message();
            stream->Close();
        });
    }

    void ProxyServer::dispatch(const std::string &nameSpace,
                               const std::shared_ptr<ClientStream> &stream,
                               const std::shared_ptr<beast::flat_buffer> &buffer,
                               const std::shared_ptr<ClientStream::HeaderReader> &header) {

        const auto &headers = header->get();
        const auto path = pathOf(std::string(headers.target()));
        const auto method = std::string(headers.method_string());

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
                        errorResponse(headers, http::status::method_not_allowed, "method not allowed for this path"));
                response->set(http::field::allow, allow);
                respond(stream, response);
                return;
            }

            log_debug << "No route for " << path;
            respond(stream, std::make_shared<http::response<http::string_body> >(
                                    errorResponse(headers, http::status::not_found, "no route for this path")));
            return;
        }
        const auto &match = *found.route;

        if (match.type == Database::Entity::EAG::RouteType::UPLOAD) {
            serveUpload(stream, buffer, header, match);
            return;
        }

        // A proxied request is read whole: it has to be forwarded, and an RFC 9421 signature over
        // it can only be checked once the body it covers is here. The limit is the gateway's own
        // rather than the route's - an upload route is the one that says how much it takes, and a
        // proxy route's backend is an application that was never going to be handed gigabytes.
        auto body = std::make_shared<ClientStream::BodyReader>(std::move(*header));
        body->body_limit(maxProxyBody());

        stream->AsyncRead(*buffer, *body, [this, stream, buffer, body, nameSpace, match](const beast::error_code &ec) {
            if (ec) {
                if (ec == http::error::body_limit) {
                    log_debug << "Request body over the limit for route " << match.routeId;
                    respond(stream, std::make_shared<http::response<http::string_body> >(
                                            errorResponse(body->get(), http::status::payload_too_large,
                                                          "request body is larger than this gateway accepts")));
                    return;
                }
                if (ec != http::error::end_of_stream) log_debug << "API gateway read failed: " << ec.message();
                return;
            }
            route(nameSpace, stream, std::make_shared<http::request<http::string_body> >(body->release()), match);
        });
    }

    ProxyServer::UploadAuth ProxyServer::authenticateUpload(const http::request<http::empty_body> &headers,
                                                            const Database::Entity::EAG::Route &match) {

        const auto authorization = std::string(headers[http::field::authorization]);

        // The signature functions are written against the request type everything else in euclid
        // uses, and what is being verified here is a request whose body has not arrived. The
        // headers are what the signature covers, so they are copied into a message of that type
        // with the body left empty - which costs nothing, and is exactly the request the caller
        // signed as far as any covered component is concerned.
        http::request<http::string_body> signable;
        signable.base() = headers.base();

        // Checked first, for the same reason Authenticate() checks it first: a signature lives in
        // its own headers and a client may present one with no Authorization header at all.
        if (Core::HttpSignature::IsSigned(signable)) {

            std::string userId;
            std::string secret;
            const auto lookupSecret = [&](const std::string &accessKeyId) -> std::optional<std::string> {
                const auto record = Core::HttpActionServer::LookupAccessKey(accessKeyId);
                if (!record.has_value()) return std::nullopt;
                userId = record->userId;
                secret = record->secretAccessKey;
                return record->secretAccessKey;
            };

            // The body is not here, so the one check that needs it is left for finishUpload() -
            // which is stronger than it sounds: the caller has committed to a specific body before
            // a byte of it has been read.
            const auto verified = Core::HttpSignature::VerifyWithoutBody(signable, lookupSecret);
            if (!verified.has_value()) {
                return {.allowed = false, .reason = "signature does not match"};
            }

            return {.allowed = true,
                    .userId = userId,
                    .credential = {.accessKeyId = verified->accessKeyId, .secretAccessKey = secret},
                    .signedDigest = verified->contentDigest};
        }

        if (constexpr std::string_view bearerPrefix = "Bearer "; authorization.starts_with(bearerPrefix)) {

            const auto token = authorization.substr(bearerPrefix.size());
            const auto subject = Core::JwtUtils::VerifyToken(token, Core::HttpActionServer::JwtSecret());
            if (!subject.has_value()) {
                return {.allowed = false, .reason = "authentication required"};
            }
            return {.allowed = true, .userId = *subject, .credential = {.bearerToken = token}};
        }

        // HTTP Basic, which the gateway verifies and which every onward call then needs a token
        // for. Obtained by logging in as them with the password they just sent - a real login,
        // not a session the gateway invented on their behalf.
        if (match.authentication == Database::Entity::EAG::RouteAuthentication::BASIC) {

            const auto userId = _basicAuth.Verify(authorization);
            if (!userId.has_value()) {
                return {.allowed = false, .reason = "authentication required"};
            }

            const auto separator = authorization.find(' ');
            const auto decoded = separator == std::string::npos
                                         ? std::string()
                                         : Core::CryptoUtils::Base64Decode(authorization.substr(separator + 1));
            const auto colon = decoded.find(':');
            if (colon == std::string::npos) {
                return {.allowed = false, .reason = "authentication required"};
            }

            const boost::json::object login{{"userId", decoded.substr(0, colon)}, {"password", decoded.substr(colon + 1)}};
            const auto answer = CallModule(_euclidGatewayPort, _euclidGatewayTls, _euclidGatewayCtx, {},
                                           {.target = "eam", .action = "login", .body = boost::json::serialize(login)});
            if (!answer.IsSuccess()) {
                log_warning << "Could not obtain a session for Basic upload by " << *userId << ", status: " << answer.status;
                return {.allowed = false, .reason = "authentication required"};
            }

            try {
                const auto token = boost::json::parse(answer.body).at("token").as_string();
                return {.allowed = true, .userId = *userId, .credential = {.bearerToken = std::string(token)}};
            } catch (const std::exception &) {
                return {.allowed = false, .reason = "authentication required"};
            }
        }

        return {.allowed = false, .reason = "authentication required"};
    }

    // Everything one upload needs between the header arriving and the last part going out. Held by
    // shared_ptr across the read loop, which is what keeps the connection and the reader alive
    // while the body is still coming.
    struct ProxyServer::Upload {

        std::shared_ptr<ClientStream> stream;
        std::shared_ptr<beast::flat_buffer> buffer;
        std::shared_ptr<ClientStream::StreamReader> reader;
        Database::Entity::EAG::Route route;

        /**
         * @brief One part's worth of bytes, reused for every part.
         *
         * @par
         * Reused rather than reallocated, and one of them rather than one per part: this is the
         * whole of what an upload costs the gateway in memory, whatever the size of the object
         * going through it.
         */
        std::vector<char> part;

        /**
         * @brief The digest of everything read so far, for comparing against the signed
         * Content-Digest once the last byte has gone past.
         */
        Core::Sha256Digest digest;

        /**
         * @brief How many bytes have arrived, for the route's own limit and for the log.
         */
        std::uint64_t received{0};

        /**
         * @brief Who the upload is, and what its calls to ESM carry.
         */
        UploadAuth auth;

        /**
         * @brief The key being written, prefix included.
         */
        std::string key;

        /**
         * @brief ESM's id for the multipart upload the parts belong to.
         */
        std::string uploadId;

        /**
         * @brief Which part goes next. ESM numbers them from one.
         */
        long partNumber{0};

        /**
         * @brief Set once a part has been refused, so the read loop stops asking for more.
         */
        bool failed{false};
    };

    void ProxyServer::serveUpload(const std::shared_ptr<ClientStream> &stream,
                                  const std::shared_ptr<beast::flat_buffer> &buffer,
                                  const std::shared_ptr<ClientStream::HeaderReader> &header,
                                  const Database::Entity::EAG::Route &match) {

        const auto &headers = header->get();

        auto auth = authenticateUpload(headers, match);
        if (!auth.allowed) {
            log_debug << "Unauthenticated upload for route " << match.routeId << ": " << auth.reason;
            auto response = std::make_shared<http::response<http::string_body> >(
                    errorResponse(headers, http::status::unauthorized, auth.reason));
            if (match.authentication == Database::Entity::EAG::RouteAuthentication::BASIC) {
                response->set(http::field::www_authenticate, R"(Basic realm="euclid", charset="UTF-8")");
            }
            respond(stream, response);
            return;
        }

        // Both of these are answered before a byte of the body is read, which is the point of
        // having matched the route first: a caller sending something this route will not take is
        // told so now rather than after spending however long it takes to send it.
        if (!match.upload.contentTypes.empty()) {
            auto contentType = std::string(headers[http::field::content_type]);
            if (const auto semicolon = contentType.find(';'); semicolon != std::string::npos) {
                contentType = contentType.substr(0, semicolon);
            }
            std::ranges::transform(contentType, contentType.begin(),
                                   [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });

            if (std::ranges::find(match.upload.contentTypes, contentType) == match.upload.contentTypes.end()) {
                log_debug << "Content type " << contentType << " not accepted by route " << match.routeId;
                respond(stream, std::make_shared<http::response<http::string_body> >(
                                        errorResponse(headers, http::status::unsupported_media_type,
                                                      "this route does not accept that content type")));
                return;
            }
        }

        // A Content-Length over the limit is refused here; a body that turns out to be longer than
        // it said is caught by the reader's own limit below. Both are needed - the header is a
        // claim, and the limit is what holds when the claim was a lie.
        if (match.upload.maxBytes > 0) {
            if (const auto length = headers[http::field::content_length]; !length.empty()) {
                try {
                    if (std::stoull(std::string(length)) > static_cast<unsigned long long>(match.upload.maxBytes)) {
                        log_debug << "Upload of " << length << " bytes over the limit for route " << match.routeId;
                        respond(stream, std::make_shared<http::response<http::string_body> >(
                                                errorResponse(headers, http::status::payload_too_large,
                                                              "body is larger than this route accepts")));
                        return;
                    }
                } catch (const std::exception &) {
                    respond(stream, std::make_shared<http::response<http::string_body> >(
                                            errorResponse(headers, http::status::bad_request, "Content-Length is not a number")));
                    return;
                }
            }
        }

        const auto resolved = ResolveUploadKey(match, headers.target());
        if (!resolved.valid) {
            respond(stream, std::make_shared<http::response<http::string_body> >(
                                    errorResponse(headers, http::status::bad_request, resolved.reason)));
            return;
        }

        auto upload = std::make_shared<Upload>();
        upload->stream = stream;
        upload->buffer = buffer;
        upload->route = match;
        upload->auth = std::move(auth);
        upload->key = resolved.key;
        upload->part.resize(static_cast<std::size_t>(match.upload.partSize));
        upload->reader = std::make_shared<ClientStream::StreamReader>(std::move(*header));

        // Staged before a byte is read, so that a caller who may not write here is told so now.
        // ESM decides that, against the caller's own grants - see ModuleCredential.
        const boost::json::object create{{"bucketErn", match.upload.bucket}, {"key", upload->key}};
        const auto created = CallModule(_euclidGatewayPort, _euclidGatewayTls, _euclidGatewayCtx, upload->auth.credential,
                                        {.target = "esm",
                                         .action = "create-upload",
                                         .body = boost::json::serialize(create),
                                         .region = !match.region.empty() ? match.region : _region,
                                         .accountId = match.accountId,
                                         .userId = upload->auth.userId,
                                         .nameSpace = match.nameSpace});
        if (!created.IsSuccess()) {
            log_warning << "Could not start upload for route " << match.routeId << ", key: " << upload->key
                        << ", status: " << created.status;
            respond(stream, std::make_shared<http::response<http::string_body> >(
                                    errorResponse(headers,
                                                  created.status == 0 ? http::status::bad_gateway
                                                                      : static_cast<http::status>(created.status),
                                                  created.status == 0 ? "the storage service could not be reached"
                                                                      : "the upload was refused")));
            return;
        }

        try {
            upload->uploadId = std::string(boost::json::parse(created.body).at("uploadId").as_string());
        } catch (const std::exception &e) {
            log_error << "create-upload answered without an uploadId: " << e.what();
            respond(stream, std::make_shared<http::response<http::string_body> >(
                                    errorResponse(headers, http::status::bad_gateway, "the storage service answered unexpectedly")));
            return;
        }

        // Past this the reader refuses to go, whatever the caller said it was sending.
        if (match.upload.maxBytes > 0) {
            upload->reader->body_limit(static_cast<std::uint64_t>(match.upload.maxBytes));
        } else {
            upload->reader->body_limit(boost::none);
        }

        readUploadPart(upload);
    }

    void ProxyServer::readUploadPart(const std::shared_ptr<Upload> &upload) {

        // Asked before reading rather than after, because a body of no bytes is already finished
        // when its headers are: reading first would wait for a chunk that is never coming and end
        // the upload on a timeout instead of on a response.
        if (upload->reader->is_done()) {
            finishUpload(upload);
            return;
        }

        // Where the next chunk goes, and how much of it there is room for. Beast writes into this
        // and tells us how much it used by how much of "size" it left.
        upload->reader->get().body().data = upload->part.data();
        upload->reader->get().body().size = upload->part.size();

        upload->stream->ExpiresAfter(kBackendTimeout);
        upload->stream->AsyncReadSome(*upload->buffer, *upload->reader, [this, upload](const beast::error_code &ec) {
            // need_buffer is not a failure: it is the reader saying it filled what it was given and
            // has more to put somewhere. Every other error ends the upload.
            if (ec && ec != http::error::need_buffer) {
                if (ec == http::error::body_limit) {
                    log_debug << "Upload over the limit for route " << upload->route.routeId;
                    respond(upload->stream, std::make_shared<http::response<http::string_body> >(
                                                    errorResponse(upload->reader->get(), http::status::payload_too_large,
                                                                  "body is larger than this route accepts")));
                    return;
                }
                if (ec != http::error::end_of_stream) {
                    log_debug << "Upload read failed for route " << upload->route.routeId << ": " << ec.message();
                }
                return;
            }

            const auto written = upload->part.size() - upload->reader->get().body().size;
            if (written > 0) {
                upload->received += written;
                upload->digest.update(std::string_view(upload->part.data(), written));

                // Sent before the next part is read, which is what keeps one part in memory at a
                // time and also what stops the gateway reading faster than ESM can store.
                if (!sendUploadPart(upload, written)) return;
            }

            readUploadPart(upload);
        });
    }

    void ProxyServer::abandonUpload(const std::shared_ptr<Upload> &upload) {

        if (upload->uploadId.empty()) return;

        const boost::json::object abort{{"uploadId", upload->uploadId}};
        if (const auto answer = CallModule(_euclidGatewayPort, _euclidGatewayTls, _euclidGatewayCtx, upload->auth.credential,
                                           {.target = "esm",
                                            .action = "abort-upload",
                                            .body = boost::json::serialize(abort),
                                            .region = !upload->route.region.empty() ? upload->route.region : _region,
                                            .accountId = upload->route.accountId,
                                            .userId = upload->auth.userId,
                                            .nameSpace = upload->route.nameSpace});
            !answer.IsSuccess()) {

            // Logged and not escalated: the caller is already being told their upload failed, and
            // telling them it also failed to tidy up would replace a clear answer with a confusing
            // one. What is left behind is scratch storage an operator can remove with
            // "esm abort-upload --upload-id", which is why the id is in this line.
            log_warning << "Could not abandon upload " << upload->uploadId << " for route " << upload->route.routeId
                        << ", status: " << answer.status;
            return;
        }
        log_info << "Abandoned upload " << upload->uploadId << " for route " << upload->route.routeId
                 << ", key: " << upload->key;
    }

    bool ProxyServer::sendUploadPart(const std::shared_ptr<Upload> &upload, const std::size_t size) {

        const auto answer = CallModule(_euclidGatewayPort, _euclidGatewayTls, _euclidGatewayCtx, upload->auth.credential,
                                       {.target = "esm",
                                        .action = "upload-part",
                                        .body = std::string(upload->part.data(), size),
                                        .contentType = "application/octet-stream",
                                        .region = !upload->route.region.empty() ? upload->route.region : _region,
                                        .accountId = upload->route.accountId,
                                        .userId = upload->auth.userId,
                                        .nameSpace = upload->route.nameSpace,
                                        .headers = {{"x-euclid-upload-id", upload->uploadId},
                                                    {"x-euclid-part-number", std::to_string(++upload->partNumber)}}});
        if (answer.IsSuccess()) return true;

        // Whatever was staged is thrown away rather than left for somebody to find: the object
        // stays as create-upload left it rather than becoming a half-written version of itself,
        // and the scratch storage goes with the upload.
        upload->failed = true;
        abandonUpload(upload);
        log_warning << "Upload part " << upload->partNumber << " refused for route " << upload->route.routeId
                    << ", key: " << upload->key << ", status: " << answer.status;

        respond(upload->stream, std::make_shared<http::response<http::string_body> >(
                                        errorResponse(upload->reader->get(),
                                                      answer.status == 0 ? http::status::bad_gateway
                                                                         : http::status::internal_server_error,
                                                      answer.status == 0 ? "the storage service could not be reached"
                                                                         : "the upload could not be stored")));
        return false;
    }

    void ProxyServer::finishUpload(const std::shared_ptr<Upload> &upload) {

        if (upload->failed) return;

        // The check deferred from authenticateUpload(): the caller committed to a digest before
        // the first byte was read, and this is where what arrived is held to it. A mismatch means
        // the body was not the one that was signed, so nothing completes and everything staged is
        // thrown away.
        if (!upload->auth.signedDigest.empty()
            && !Core::HttpSignature::DigestMatches(upload->auth.signedDigest, upload->digest.raw())) {

            log_warning << "Upload body does not match the signed digest, route: " << upload->route.routeId
                        << ", key: " << upload->key << ", bytes: " << upload->received;
            abandonUpload(upload);
            respond(upload->stream, std::make_shared<http::response<http::string_body> >(
                                            errorResponse(upload->reader->get(), http::status::bad_request,
                                                          "the body does not match the signed Content-Digest")));
            return;
        }

        // A body of no bytes still has to arrive as one part: complete-upload refuses an upload
        // with none at all ("Upload has no parts"), so a zero-length PUT would otherwise be the
        // one size of file this route cannot take. The CLI's upload-file sends the same empty
        // part for the same reason - an empty object is a real object, and the key should hold it.
        if (upload->partNumber == 0 && !sendUploadPart(upload, 0)) return;

        const boost::json::object complete{{"uploadId", upload->uploadId}};
        const auto answer = CallModule(_euclidGatewayPort, _euclidGatewayTls, _euclidGatewayCtx, upload->auth.credential,
                                       {.target = "esm",
                                        .action = "complete-upload",
                                        .body = boost::json::serialize(complete),
                                        .region = !upload->route.region.empty() ? upload->route.region : _region,
                                        .accountId = upload->route.accountId,
                                        .userId = upload->auth.userId,
                                        .nameSpace = upload->route.nameSpace});
        if (!answer.IsSuccess()) {
            log_warning << "Could not complete upload for route " << upload->route.routeId << ", key: " << upload->key
                        << ", status: " << answer.status;
            abandonUpload(upload);
            respond(upload->stream, std::make_shared<http::response<http::string_body> >(
                                            errorResponse(upload->reader->get(),
                                                          answer.status == 0 ? http::status::bad_gateway
                                                                             : http::status::internal_server_error,
                                                          "the upload could not be stored")));
            return;
        }

        log_info << "Upload stored on route " << upload->route.routeId << ", key: " << upload->key
                 << ", bytes: " << upload->received << ", parts: " << upload->partNumber;

        auto response = std::make_shared<http::response<http::string_body> >(http::status::created,
                                                                             upload->reader->get().version());
        response->set(http::field::content_type, "application/json");
        response->set(http::field::location, upload->route.path + "/" + upload->key);
        response->body() = boost::json::serialize(boost::json::object{
                {"bucket", upload->route.upload.bucket},
                {"key", upload->key},
                {"size", static_cast<std::int64_t>(upload->received)}});
        response->prepare_payload();
        respond(upload->stream, response);
    }

    void ProxyServer::route(const std::string &nameSpace,
                            const std::shared_ptr<ClientStream> &stream,
                            const std::shared_ptr<http::request<http::string_body> > &request,
                            const Database::Entity::EAG::Route &matched) {

        const auto path = pathOf(std::string(request->target()));
        const auto &match = matched;

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
            if (const auto &region = !match.region.empty() ? match.region : _region; !region.empty()) {
                request->set("x-euclid-region", region);
            }
        }
        if ((*request)["x-euclid-namespace"].empty()) {
            if (const auto &ns = !match.nameSpace.empty() ? match.nameSpace : nameSpace; !ns.empty()) {
                request->set("x-euclid-namespace", ns);
            }
        }

        // Checked here, before a backend is chosen, so an unauthenticated caller never causes a
        // connection to an application at all.
        if (match.authentication == Database::Entity::EAG::RouteAuthentication::EUCLID) {
            if (const auto auth = Core::HttpActionServer::Authenticate(*request); !auth.subject.has_value()) {
                log_debug << "Unauthenticated request for " << path << ", route: " << match.routeId;
                respond(stream, std::make_shared<http::response<http::string_body> >(
                                        errorResponse(*request, http::status::unauthorized,
                                                      auth.denialReason.empty() ? "authentication required" : auth.denialReason)));
                return;
            }
        } else if (match.authentication == Database::Entity::EAG::RouteAuthentication::BASIC) {
            if (!_basicAuth.Verify(std::string((*request)[http::field::authorization])).has_value()) {
                log_debug << "Basic authentication required for " << path << ", route: " << match.routeId;

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
        if (!match.moduleTarget.empty()) {
            if (_euclidGatewayTls) {
                proxyToTls(stream, request, _euclidGatewayPort, match.routeId, match.moduleTarget, match.moduleAction);
            } else {
                proxyTo(stream, request, _euclidGatewayPort, match.routeId, match.moduleTarget, match.moduleAction);
            }
            return;
        }

        const auto port = _backends.next(ApplicationRef{.accountId = match.accountId,
                                                        .nameSpace = match.nameSpace,
                                                        .applicationId = match.applicationId});
        if (!port.has_value()) {
            // The route is configured and the application is simply not there: scaled to zero,
            // still starting, or never given a port. Said as 503 rather than 404, because the
            // resource exists and the caller may reasonably try again.
            log_warning << "No running instance for application " << match.applicationId << ", route: " << match.routeId;
            respond(stream, std::make_shared<http::response<http::string_body> >(
                                    errorResponse(*request, http::status::service_unavailable,
                                                  "no running instance of application '" + match.applicationId + "'")));
            return;
        }

        proxyTo(stream, request, *port, match.routeId, {}, {});
    }

    // The request as the backend should see it: the caller's own, with the hop rewritten and -
    // for a module route - the target and action the route names. Shared so the plain and TLS
    // paths cannot drift apart about what they forward.
    static std::shared_ptr<http::request<http::string_body> > forwardedRequest(
            const std::shared_ptr<ClientStream> &stream,
            const std::shared_ptr<http::request<http::string_body> > &request,
            const int port, const std::string &euclidTarget, const std::string &euclidAction) {

        auto forwarded = std::make_shared<http::request<http::string_body> >(*request);
        forwarded->set(http::field::host, "127.0.0.1:" + std::to_string(port));

        // What the caller spoke, not what this hop speaks. An application that builds an absolute
        // URL - a redirect, a link in a response - would otherwise send an https caller back to
        // itself over http, and a browser that followed it would report the downgrade.
        forwarded->set("X-Forwarded-Proto", stream->IsTls() ? "https" : "http");

        if (const auto peer = stream->RemoteEndpoint(); peer.has_value()) {
            forwarded->set("X-Forwarded-For", peer->address().to_string());
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

    void ProxyServer::proxyToTls(const std::shared_ptr<ClientStream> &stream,
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

    void ProxyServer::proxyTo(const std::shared_ptr<ClientStream> &stream,
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
