// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/7/26.
//

// Euclid includes
#include <ClientStream.h>

namespace Euclid::EAG {

    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = boost::beast::http;
    using tcp = boost::asio::ip::tcp;

    ClientStream::ClientStream(tcp::socket socket)
        : _stream(std::in_place_type<beast::tcp_stream>, std::move(socket)) {}

    ClientStream::ClientStream(tcp::socket socket, std::shared_ptr<asio::ssl::context> context)
        : _stream(std::in_place_type<beast::ssl_stream<beast::tcp_stream> >, std::move(socket), *context),
          _context(std::move(context)) {}

    bool ClientStream::IsTls() const {
        return std::holds_alternative<beast::ssl_stream<beast::tcp_stream> >(_stream);
    }

    void ClientStream::ExpiresAfter(const std::chrono::seconds timeout) {
        std::visit([timeout](auto &stream) { beast::get_lowest_layer(stream).expires_after(timeout); }, _stream);
    }

    void ClientStream::Handshake(std::function<void(beast::error_code)> done) {

        if (auto *tls = std::get_if<beast::ssl_stream<beast::tcp_stream> >(&_stream)) {
            tls->async_handshake(asio::ssl::stream_base::server,
                                 [done = std::move(done)](const beast::error_code &ec) { done(ec); });
            return;
        }

        // Nothing to negotiate, but the callback is still made rather than the caller being
        // returned to: every path out of Handshake() then looks the same, and the read that
        // follows is written once.
        auto &plain = std::get<beast::tcp_stream>(_stream);
        asio::post(plain.get_executor(), [done = std::move(done)] { done({}); });
    }

    void ClientStream::AsyncRead(beast::flat_buffer &buffer, http::request<http::string_body> &request,
                                 std::function<void(beast::error_code)> done) {
        std::visit([&buffer, &request, done = std::move(done)](auto &stream) mutable {
            http::async_read(stream, buffer, request,
                             [done = std::move(done)](const beast::error_code &ec, std::size_t) { done(ec); });
        },
                   _stream);
    }

    void ClientStream::AsyncWrite(http::response<http::string_body> &response, std::function<void(beast::error_code)> done) {
        std::visit([&response, done = std::move(done)](auto &stream) mutable {
            http::async_write(stream, response,
                              [done = std::move(done)](const beast::error_code &ec, std::size_t) { done(ec); });
        },
                   _stream);
    }

    void ClientStream::Close() {

        beast::error_code ignored;
        if (auto *plain = std::get_if<beast::tcp_stream>(&_stream)) {
            plain->socket().shutdown(tcp::socket::shutdown_send, ignored);
            return;
        }
        beast::get_lowest_layer(std::get<beast::ssl_stream<beast::tcp_stream> >(_stream)).close();
    }

    std::optional<tcp::endpoint> ClientStream::RemoteEndpoint() const {
        return std::visit([](const auto &stream) -> std::optional<tcp::endpoint> {
            beast::error_code ec;
            // const_cast: beast's lowest layer accessor has no const overload, and nothing here
            // changes the stream - remote_endpoint() only reads what the socket already knows.
            auto &lowest = beast::get_lowest_layer(const_cast<std::remove_const_t<std::remove_reference_t<decltype(stream)> > &>(stream));
            const auto endpoint = lowest.socket().remote_endpoint(ec);
            if (ec) return std::nullopt;
            return endpoint;
        },
                          _stream);
    }

}// namespace Euclid::EAG
