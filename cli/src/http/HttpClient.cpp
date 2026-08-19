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

        http::request<http::string_body> BuildRequest(const HttpMethod method, const std::string &host, const std::string &target, const std::string &action, const boost::json::value *body, const Credentials::Entry &authentication) {

            http::request<http::string_body> request(ToVerb(method), "/", 11);
            request.set(http::field::host, host);
            request.set(http::field::user_agent, "euclid-cli");
            request.set(http::field::accept, "application/json");
            request.set("x-euclid-target", target);
            request.set("x-euclid-action", action);
            request.set("x-euclid-region", authentication.region);
            request.set("x-euclid-account-id", authentication.accountId);
            request.set("x-euclid-user-id", authentication.userId);
            if (body != nullptr) {
                request.set(http::field::content_type, "application/json");
                request.body() = boost::json::serialize(*body);
                request.prepare_payload();
            }

            // Service calls (everything but the access module itself) sign with SigV4 when an
            // access key is configured, matching how real AWS SDKs authenticate; access-module
            // calls (login/register/access-key management) always use the bearer token, since
            // there's no access key yet at login time and key management is euclid's own session
            // auth, not an euclid service call.
            if (target != "access" && !authentication.accessKeyId.empty() && !authentication.secretAccessKey.empty()) {
                Core::SigV4::Sign(request, authentication.accessKeyId, authentication.secretAccessKey, authentication.region, target);
            } else if (!authentication.token.empty()) {
                request.set(http::field::authorization, "Bearer " + authentication.token);
            }
            return request;
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

        template<typename Stream>
        HttpResponse Execute(Stream &stream, const http::request<http::string_body> &request) {
            http::write(stream, request);

            beast::flat_buffer buffer;
            http::response<http::string_body> response;
            http::read(stream, buffer, response);

            return ToHttpResponse(response);
        }

        HttpResponse SendPlain(asio::io_context &ioc, const tcp::resolver::results_type &endpoints, const http::request<http::string_body> &request) {
            beast::tcp_stream stream(ioc);
            stream.expires_after(kTimeout);
            stream.connect(endpoints);

            const HttpResponse response = Execute(stream, request);

            beast::error_code ec;
            std::ignore = stream.socket().shutdown(tcp::socket::shutdown_both, ec);
            return response;
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
            beast::get_lowest_layer(stream).connect(endpoints);
            stream.handshake(ssl::stream_base::client);

            const HttpResponse response = Execute(stream, request);

            beast::error_code ec;
            std::ignore = stream.shutdown(ec);
            return response;
        }

    }

    bool HttpResponse::IsSuccess() const {
        return statusCode >= 200 && statusCode < 300;
    }

    HttpClient::HttpClient(std::string endpoint, Credentials::Entry authentication, std::string caCertPath) : _endpoint(std::move(endpoint)), _authentication(std::move(authentication)), _caCertPath(std::move(caCertPath)) {}

    HttpResponse HttpClient::Send(const HttpMethod method, const std::string &target, const std::string &action, const boost::json::value *body) const {
        const boost::system::result<boost::urls::url_view> parsed = boost::urls::parse_uri(_endpoint);
        if (!parsed) {
            throw std::runtime_error("invalid endpoint '" + _endpoint + "': " + parsed.error().message());
        }

        const boost::urls::url_view &url = *parsed;
        const auto scheme = url.scheme().empty() ? "http" : std::string(url.scheme());
        const auto host = std::string(url.host());
        const auto port = url.has_port() ? std::string(url.port()) : (scheme == "https" ? "443" : "80");

        try {
            asio::io_context ioc;
            tcp::resolver resolver(ioc);
            const tcp::resolver::results_type endpoints = resolver.resolve(host, port);

            const http::request<http::string_body> request = BuildRequest(method, host, target, action, body, _authentication);

            if (scheme == "https") {
                return SendTls(ioc, endpoints, host, request, _caCertPath);
            }
            return SendPlain(ioc, endpoints, request);
        } catch (const boost::system::system_error &ex) {
            throw std::runtime_error("failed to reach " + _endpoint + " (target=" + target + ", action=" + action + "): " + ex.code().message());
        }
    }

    HttpResponse HttpClient::Get(const std::string &target, const std::string &action) const {
        return Send(HttpMethod::GET, target, action, nullptr);
    }

    HttpResponse HttpClient::Post(const std::string &target, const std::string &action, const boost::json::value &body) const {
        return Send(HttpMethod::POST, target, action, &body);
    }

    HttpResponse HttpClient::Put(const std::string &target, const std::string &action, const boost::json::value &body) const {
        return Send(HttpMethod::PUT, target, action, &body);
    }

    HttpResponse HttpClient::Delete(const std::string &target, const std::string &action) const {
        return Send(HttpMethod::DEL, target, action, nullptr);
    }

}