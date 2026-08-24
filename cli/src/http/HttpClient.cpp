#include <euclid/cli/http/HttpClient.h>

// Euclid includes
#include <euclid/core/SigV4.h>

namespace Euclid::CLI {

    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace ssl = asio::ssl;
    using tcp = asio::ip::tcp;

    namespace {

        constexpr std::chrono::seconds kTimeout{30};

        http::verb ToVerb(const HttpMethod method) {
            switch (method) {
                case HttpMethod::GET:
                    return http::verb::get;
                case HttpMethod::POST:
                    return http::verb::post;
                case HttpMethod::PUT:
                    return http::verb::put;
                case HttpMethod::DEL:
                    return http::verb::delete_;
            }
            throw std::runtime_error("unknown HTTP method");
        }

        void SetCommonHeaders(http::request<http::string_body> &request, const std::string &host, const std::string &target, const std::string &action, const Credentials::Entry &authentication) {
            request.set(http::field::host, host);
            request.set(http::field::user_agent, "euclid-cli");
            request.set(http::field::accept, "application/json");
            request.set("x-euclid-target", target);
            request.set("x-euclid-action", action);
            request.set("x-euclid-region", authentication.region);
            request.set("x-euclid-account-id", authentication.accountId);
            request.set("x-euclid-user-id", authentication.userId);
        }

        // Service calls (everything but the access module itself) sign with SigV4 when an
        // access key is configured, matching how real AWS SDKs authenticate; access-module
        // calls (login/register/access-key management) always use the bearer token, since
        // there's no access key yet at login time and key management is euclid's own session
        // auth, not an euclid service call.
        void AuthenticateRequest(http::request<http::string_body> &request, const std::string &target, const Credentials::Entry &authentication) {
            if (target != "eam" && !authentication.accessKeyId.empty() && !authentication.secretAccessKey.empty()) {
                Core::SigV4::Sign(request, authentication.accessKeyId, authentication.secretAccessKey, authentication.region, target);
            } else if (!authentication.token.empty()) {
                request.set(http::field::authorization, "Bearer " + authentication.token);
            }
        }

        http::request<http::string_body> BuildRequest(const HttpMethod method, const std::string &host, const std::string &target, const std::string &action, const boost::json::value *body, const std::vector<std::pair<std::string, std::string> > &extraHeaders, const Credentials::Entry &authentication) {

            http::request<http::string_body> request(ToVerb(method), "/", 11);
            SetCommonHeaders(request, host, target, action, authentication);
            for (const auto &[name, value]: extraHeaders) {
                request.set(name, value);
            }
            if (body != nullptr) {
                request.set(http::field::content_type, "application/json");
                request.body() = boost::json::serialize(*body);
                request.prepare_payload();
            }
            AuthenticateRequest(request, target, authentication);
            return request;
        }

        // Bodies here are opaque bytes rather than a boost::json::value - request-specific
        // metadata that would normally be a JSON field (e.g. upload ID, part number) travels in
        // extraHeaders instead, alongside the same x-euclid-* headers every other request sends.
        http::request<http::string_body> BuildBinaryRequest(const std::string &host, const std::string &target, const std::string &action, const std::vector<std::pair<std::string, std::string> > &extraHeaders, const std::string &data, const Credentials::Entry &authentication) {

            http::request<http::string_body> request(http::verb::post, "/", 11);
            SetCommonHeaders(request, host, target, action, authentication);
            for (const auto &[name, value]: extraHeaders) {
                request.set(name, value);
            }
            request.set(http::field::content_type, "application/octet-stream");
            request.body() = data;
            request.prepare_payload();
            AuthenticateRequest(request, target, authentication);
            return request;
        }

        struct EndpointParts {
            std::string scheme;
            std::string host;
            std::string port;
        };

        EndpointParts ParseEndpoint(const std::string &endpoint) {
            const boost::system::result<boost::urls::url_view> parsed = boost::urls::parse_uri(endpoint);
            if (!parsed) {
                throw std::runtime_error("invalid endpoint '" + endpoint + "': " + parsed.error().message());
            }

            const boost::urls::url_view &url = *parsed;
            const auto scheme = url.scheme().empty() ? "http" : std::string(url.scheme());
            const auto host = std::string(url.host());
            const auto port = url.has_port() ? std::string(url.port()) : (scheme == "https" ? "443" : "80");
            return {scheme, host, port};
        }

        HttpResponse ToHttpResponse(const http::response<http::string_body> &response) {
            HttpResponse result;
            result.statusCode = static_cast<int>(response.result_int());
            if (!response.body().empty()) {
                boost::system::error_code ec;
                result.body = boost::json::parse(response.body(), ec);
                if (ec) {
                    result.body = boost::json::value(response.body());
                }
            }
            return result;
        }

        // Boost.Beast's tcp_stream/ssl_stream timeout support (armed below via expires_after)
        // only takes effect for ASYNCHRONOUS operations - the synchronous connect/write/read
        // overloads silently ignore it and can block forever. That's what let a single orphaned
        // connection (e.g. from the gateway's autoscaler killing an instance mid-request) hang an
        // entire multi-part upload with no error and no retry. Running the whole exchange as one
        // chained async pipeline, driven by a single ioc.run(), makes the configured timeout real:
        // if any step stalls past kTimeout, the stream's internal timer cancels it and the pending
        // handler gets an error instead of hanging.

        HttpResponse SendPlain(asio::io_context &ioc, const tcp::resolver::results_type &endpoints, const http::request<http::string_body> &request) {
            beast::tcp_stream stream(ioc);
            stream.expires_after(kTimeout);

            beast::error_code opEc;
            beast::flat_buffer buffer;
            http::response<http::string_body> response;

            stream.async_connect(endpoints, [&](const beast::error_code &ec, const tcp::endpoint &) {
                if (ec) {
                    opEc = ec;
                    return;
                }
                http::async_write(stream, request, [&](const beast::error_code &writeEc, std::size_t) {
                    if (writeEc) {
                        opEc = writeEc;
                        return;
                    }
                    http::async_read(stream, buffer, response, [&](const beast::error_code &readEc, std::size_t) {
                        opEc = readEc;
                    });
                });
            });

            ioc.run();

            beast::error_code ec;
            std::ignore = stream.socket().shutdown(tcp::socket::shutdown_both, ec);

            if (opEc) throw boost::system::system_error(opEc);
            return ToHttpResponse(response);
        }

        HttpResponse SendTls(asio::io_context &ioc, const tcp::resolver::results_type &endpoints, const std::string &host, const http::request<http::string_body> &request, const std::string &caCertPath) {
            ssl::context ctx(ssl::context::tlsv12_client);
            ctx.set_default_verify_paths();
            if (!caCertPath.empty()) {
                ctx.load_verify_file(caCertPath);
            }

            beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
            if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
                throw std::runtime_error("failed to set TLS server name for " + host);
            }
            stream.set_verify_mode(ssl::verify_peer);
            beast::get_lowest_layer(stream).expires_after(kTimeout);

            beast::error_code opEc;
            beast::flat_buffer buffer;
            http::response<http::string_body> response;

            beast::get_lowest_layer(stream).async_connect(endpoints, [&](const beast::error_code &ec, const tcp::endpoint &) {
                if (ec) {
                    opEc = ec;
                    return;
                }
                stream.async_handshake(ssl::stream_base::client, [&](const beast::error_code &handshakeEc) {
                    if (handshakeEc) {
                        opEc = handshakeEc;
                        return;
                    }
                    http::async_write(stream, request, [&](const beast::error_code &writeEc, std::size_t) {
                        if (writeEc) {
                            opEc = writeEc;
                            return;
                        }
                        http::async_read(stream, buffer, response, [&](const beast::error_code &readEc, std::size_t) {
                            opEc = readEc;
                        });
                    });
                });
            });

            ioc.run();

            beast::error_code ec;
            std::ignore = stream.shutdown(ec);

            if (opEc) throw boost::system::system_error(opEc);
            return ToHttpResponse(response);
        }

    }

    bool HttpResponse::IsSuccess() const {
        return statusCode >= 200 && statusCode < 300;
    }

    HttpClient::HttpClient(std::string endpoint, Credentials::Entry authentication, std::string caCertPath) : _endpoint(std::move(endpoint)), _authentication(std::move(authentication)), _caCertPath(std::move(caCertPath)) {}

    HttpResponse HttpClient::Transmit(const std::string &target, const std::string &action, const http::request<http::string_body> &request) const {
        const auto [scheme, host, port] = ParseEndpoint(_endpoint);

        try {
            asio::io_context ioc;
            tcp::resolver resolver(ioc);
            const tcp::resolver::results_type endpoints = resolver.resolve(host, port);

            if (scheme == "https") {
                return SendTls(ioc, endpoints, host, request, _caCertPath);
            }
            return SendPlain(ioc, endpoints, request);
        } catch (const boost::system::system_error &ex) {
            throw std::runtime_error("failed to reach " + _endpoint + " (target=" + target + ", action=" + action + "): " + ex.code().message());
        }
    }

    HttpResponse HttpClient::Send(const HttpMethod method, const std::string &target, const std::string &action, const boost::json::value *body, const std::vector<std::pair<std::string, std::string> > &extraHeaders) const {
        const auto [scheme, host, port] = ParseEndpoint(_endpoint);
        const http::request<http::string_body> request = BuildRequest(method, host, target, action, body, extraHeaders, _authentication);
        return Transmit(target, action, request);
    }

    HttpResponse HttpClient::Get(const std::string &target, const std::string &action) const {
        return Send(HttpMethod::GET, target, action, nullptr);
    }

    HttpResponse HttpClient::Post(const std::string &target, const std::string &action, const boost::json::value &body, const std::vector<std::pair<std::string, std::string> > &extraHeaders) const {
        return Send(HttpMethod::POST, target, action, &body, extraHeaders);
    }

    HttpResponse HttpClient::Put(const std::string &target, const std::string &action, const boost::json::value &body) const {
        return Send(HttpMethod::PUT, target, action, &body);
    }

    HttpResponse HttpClient::Delete(const std::string &target, const std::string &action) const {
        return Send(HttpMethod::DEL, target, action, nullptr);
    }

    HttpResponse HttpClient::PostBinary(const std::string &target, const std::string &action, const std::vector<std::pair<std::string, std::string> > &extraHeaders, const std::string &data) const {
        const auto [scheme, host, port] = ParseEndpoint(_endpoint);
        const http::request<http::string_body> request = BuildBinaryRequest(host, target, action, extraHeaders, data, _authentication);
        return Transmit(target, action, request);
    }

}