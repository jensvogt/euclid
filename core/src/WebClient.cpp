// C++ includes
#include <chrono>

// Boost includes
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/url.hpp>

// Euclid includes
#include <euclid/core/Version.h>
#include <euclid/core/WebClient.h>

namespace Euclid::Core {

    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace net = boost::asio;
    namespace ssl = net::ssl;
    using tcp = net::ip::tcp;

    namespace {

        // Both halves of the exchange, once there is a stream to run it on. Everything it captures
        // outlives it: the caller runs the io_context to completion before looking at any of it.
        template<class Stream>
        void writeAndRead(Stream &stream, http::request<http::string_body> &req, http::response<http::string_body> &res,
                          beast::flat_buffer &buffer, std::string &failure, bool &completed) {

            http::async_write(stream, req, [&](const beast::error_code &writeEc, std::size_t) {
                if (writeEc) {
                    failure = "write failed: " + writeEc.message();
                    return;
                }
                http::async_read(stream, buffer, res, [&](const beast::error_code &readEc, std::size_t) {
                    if (readEc) {
                        failure = "read failed: " + readEc.message();
                        return;
                    }
                    completed = true;
                });
            });
        }

    }// namespace

    WebResponse WebFetch(const std::string &url, const WebRequestOptions &options) {

        const auto parsed = boost::urls::parse_uri(url);
        if (!parsed) throw WebError("Malformed URL: " + url);

        const auto view = *parsed;
        const std::string scheme(view.scheme());
        const std::string host(view.host());
        if (host.empty()) throw WebError("URL has no host: " + url);
        if (scheme != "https" && scheme != "http") throw WebError("Unsupported URL scheme: " + url);

        std::string port(view.port());
        if (port.empty()) port = scheme == "https" ? "443" : "80";

        std::string target(view.encoded_path());
        if (target.empty()) target = "/";
        if (!view.encoded_query().empty()) target += "?" + std::string(view.encoded_query());

        http::request<http::string_body> req{http::string_to_verb(options.method), target, 11};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, std::string("euclid/") + APP_VERSION);
        req.set(http::field::accept, "application/json");
        if (!options.authorization.empty()) req.set(http::field::authorization, options.authorization);
        for (const auto &[name, value]: options.headers) req.set(name, value);
        if (!options.body.empty()) {
            req.set(http::field::content_type, options.contentType);
            req.body() = options.body;
        }
        req.prepare_payload();

        net::io_context ioc;
        tcp::resolver resolver{ioc};
        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        std::string failure;
        bool completed = false;

        if (scheme == "https") {

            ssl::context ctx{ssl::context::tls_client};
            ctx.set_verify_mode(ssl::verify_peer);
            ctx.set_default_verify_paths();
            if (!options.caCertFile.empty()) {
                beast::error_code ec;
                ctx.load_verify_file(options.caCertFile, ec);
                if (ec) throw WebError("Could not load CA certificate " + options.caCertFile + ": " + ec.message());
            }

            beast::ssl_stream<beast::tcp_stream> stream{ioc, ctx};

            // Without SNI a server fronted by a shared TLS terminator answers with the wrong
            // certificate, and the verification below then fails for a reason that has nothing to
            // do with what is actually wrong.
            if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
                throw WebError("Could not set TLS server name for " + host);
            }
            stream.set_verify_callback(ssl::host_name_verification(host));

            beast::get_lowest_layer(stream).expires_after(options.timeout);
            resolver.async_resolve(host, port, [&](const beast::error_code &resolveEc, const tcp::resolver::results_type &results) {
                if (resolveEc) {
                    failure = "could not resolve " + host + ": " + resolveEc.message();
                    return;
                }
                beast::get_lowest_layer(stream).async_connect(results, [&](const beast::error_code &connectEc, const tcp::endpoint &) {
                    if (connectEc) {
                        failure = "could not connect to " + host + ": " + connectEc.message();
                        return;
                    }
                    stream.async_handshake(ssl::stream_base::client, [&](const beast::error_code &handshakeEc) {
                        if (handshakeEc) {
                            failure = "TLS handshake with " + host + " failed: " + handshakeEc.message();
                            return;
                        }
                        writeAndRead(stream, req, res, buffer, failure, completed);
                    });
                });
            });

            ioc.run();
            beast::error_code ignored;
            beast::get_lowest_layer(stream).socket().shutdown(tcp::socket::shutdown_both, ignored);

        } else {

            // Plain HTTP is not a way to talk to an identity provider in earnest - a client secret
            // and a password do not belong on an unencrypted connection - but a test double runs on
            // loopback, and refusing the scheme outright would mean this code could only ever be
            // exercised against a real server.
            beast::tcp_stream stream{ioc};
            stream.expires_after(options.timeout);
            resolver.async_resolve(host, port, [&](const beast::error_code &resolveEc, const tcp::resolver::results_type &results) {
                if (resolveEc) {
                    failure = "could not resolve " + host + ": " + resolveEc.message();
                    return;
                }
                stream.async_connect(results, [&](const beast::error_code &connectEc, const tcp::endpoint &) {
                    if (connectEc) {
                        failure = "could not connect to " + host + ": " + connectEc.message();
                        return;
                    }
                    writeAndRead(stream, req, res, buffer, failure, completed);
                });
            });

            ioc.run();
            beast::error_code ignored;
            stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
        }

        if (!failure.empty()) throw WebError(failure);
        if (!completed) throw WebError("No response from " + host + " within " + std::to_string(options.timeout.count()) + "s");

        return {.status = static_cast<int>(res.result_int()), .body = res.body()};
    }

}// namespace Euclid::Core
