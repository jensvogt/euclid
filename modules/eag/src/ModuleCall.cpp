// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/20/26.
//

// C++ includes
#include <string>

// Boost includes
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

// Euclid includes
#include <ModuleCall.h>
#include <euclid/core/HttpSignature.h>
#include <euclid/core/LogStream.h>

namespace Euclid::EAG {

    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = boost::beast::http;
    using tcp = boost::asio::ip::tcp;

    namespace {

        // Long enough for one part of an upload to be written to disk by ESM and acknowledged, and
        // short enough that a module that has stopped answering does not hold a gateway worker for
        // the rest of the day.
        constexpr auto kModuleTimeout = std::chrono::seconds(120);

        http::request<http::string_body> buildRequest(const ModuleCredential &credential,
                                                      const ModuleCallOptions &options,
                                                      const unsigned short port) {

            http::request<http::string_body> req(http::verb::post, "/", 11);
            req.set(http::field::host, "127.0.0.1:" + std::to_string(port));
            req.set(http::field::content_type, options.contentType);
            req.set("x-euclid-target", options.target);
            req.set("x-euclid-action", options.action);
            if (!options.region.empty()) req.set("x-euclid-region", options.region);
            if (!options.accountId.empty()) req.set("x-euclid-account-id", options.accountId);
            if (!options.userId.empty()) req.set("x-euclid-user-id", options.userId);
            if (!options.nameSpace.empty()) req.set("x-euclid-namespace", options.nameSpace);
            for (const auto &[name, value]: options.headers) req.set(name, value);

            req.body() = options.body;
            req.prepare_payload();

            // Signed last, after every header it covers is set - including the ones just added
            // above, which is why this cannot be done by whatever builds the body.
            if (!credential.bearerToken.empty()) {
                req.set(http::field::authorization, "Bearer " + credential.bearerToken);
            } else if (!credential.accessKeyId.empty()) {
                Core::HttpSignature::Sign(req, credential.accessKeyId, credential.secretAccessKey);
            }
            // Neither, and the request goes unauthenticated - which is right for exactly one
            // call, "eam login", whose whole purpose is to be made by somebody who has no
            // credential yet. Every other action refuses it, which is what should happen.
            return req;
        }

        template<class Stream>
        ModuleResponse exchange(Stream &stream, const http::request<http::string_body> &req) {
            http::write(stream, req);

            beast::flat_buffer buffer;
            http::response<http::string_body> res;
            http::read(stream, buffer, res);

            return {.status = static_cast<int>(res.result_int()), .body = res.body()};
        }

    }// namespace

    ModuleResponse CallModule(const unsigned short port, const bool tls, asio::ssl::context &context,
                              const ModuleCredential &credential, const ModuleCallOptions &options) {

        try {
            const auto req = buildRequest(credential, options, port);

            asio::io_context ioc;
            const tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), port);

            if (!tls) {
                beast::tcp_stream stream(ioc);
                stream.expires_after(kModuleTimeout);
                stream.connect(endpoint);

                auto response = exchange(stream, req);

                beast::error_code ignored;
                stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
                return response;
            }

            beast::ssl_stream<beast::tcp_stream> stream(ioc, context);

            // Not set for the loopback address: SNI carries a host name, and an IP address is not
            // one. The gateway's certificate is trusted by having been loaded as an anchor rather
            // than by matching a name - see the ProxyServer constructor.
            beast::get_lowest_layer(stream).expires_after(kModuleTimeout);
            beast::get_lowest_layer(stream).connect(endpoint);
            stream.handshake(asio::ssl::stream_base::client);

            auto response = exchange(stream, req);

            beast::error_code ignored;
            beast::get_lowest_layer(stream).socket().shutdown(tcp::socket::shutdown_both, ignored);
            return response;

        } catch (const std::exception &e) {
            // A status of zero says the module was never reached, which is a different thing from
            // a module that answered with a failure - the caller of an upload has to be able to
            // tell "your object was refused" from "the storage service is not there".
            log_warning << "Module call failed, target: " << options.target << ", action: " << options.action
                        << ", error: " << e.what();
            return {.status = 0, .body = e.what()};
        }
    }

}// namespace Euclid::EAG
