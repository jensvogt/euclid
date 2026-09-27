// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/7/26.
//

// C++ includes
#include <array>
#include <functional>
#include <memory>
#include <utility>

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

    void ClientStream::AsyncReadHeader(beast::flat_buffer &buffer, HeaderReader &reader,
                                       std::function<void(beast::error_code)> done) {
        std::visit([&buffer, &reader, done = std::move(done)](auto &stream) mutable {
            http::async_read_header(stream, buffer, reader,
                                    [done = std::move(done)](const beast::error_code &ec, std::size_t) { done(ec); });
        },
                   _stream);
    }

    void ClientStream::AsyncRead(beast::flat_buffer &buffer, BodyReader &reader,
                                 std::function<void(beast::error_code)> done) {
        std::visit([&buffer, &reader, done = std::move(done)](auto &stream) mutable {
            http::async_read(stream, buffer, reader,
                             [done = std::move(done)](const beast::error_code &ec, std::size_t) { done(ec); });
        },
                   _stream);
    }

    void ClientStream::AsyncReadSome(beast::flat_buffer &buffer, StreamReader &reader,
                                     std::function<void(beast::error_code)> done) {
        std::visit([&buffer, &reader, done = std::move(done)](auto &stream) mutable {
            http::async_read_some(stream, buffer, reader,
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

    namespace {
        // How long a caller who ignores the refusal is read for before the socket is dropped on
        // them. nginx's lingering_timeout, and bounded the same way - by time rather than by bytes.
        //
        // A byte budget was the wrong bound and was tried first: a caller refused four megabytes in
        // still has most of them to send, so a cap of a few hundred kilobytes ends the drain with
        // the connection full of unsent body, which is the reset this exists to avoid. Time is what
        // actually protects the gateway here, and it protects it in the case that matters - a
        // caller who has stopped listening and keeps writing - rather than in the ordinary one.
        //
        // Nothing is held while this runs but the socket and one buffer: the reads are asynchronous,
        // so no worker waits out the five seconds.
        constexpr auto kLingerTimeout = std::chrono::seconds(5);
    }// namespace

    struct ClientStream::Drain {
        std::array<char, 16 * 1024> scratch{};
        std::size_t read{0};
    };

    void ClientStream::LingeringClose() {

        auto *plain = std::get_if<beast::tcp_stream>(&_stream);
        if (!plain) {
            // TLS is closed as Close() closes it, and for the reason given there: a close_notify
            // waits on a peer that has stopped listening. Reading ciphertext only to discard it
            // would mean decrypting it first, which is work done purely to throw away.
            Close();
            return;
        }

        // The send side first, which tells the caller there will be nothing more from here. The
        // receive side stays open, which is the whole point.
        beast::error_code ignored;
        plain->socket().shutdown(tcp::socket::shutdown_send, ignored);
        plain->expires_after(kLingerTimeout);

        drainNext(std::make_shared<Drain>());
    }

    void ClientStream::drainNext(const std::shared_ptr<Drain> &drain) {

        auto *plain = std::get_if<beast::tcp_stream>(&_stream);
        if (!plain) {
            Close();
            return;
        }

        // shared_from_this, because whoever asked for this close has already answered its own
        // caller and let go of its reference - the reads that follow are the only thing keeping
        // the connection alive, and they have to keep it alive themselves.
        plain->async_read_some(asio::buffer(drain->scratch),
                               [self = shared_from_this(), drain](const beast::error_code &ec, const std::size_t bytes) {
                                   drain->read += bytes;

                                   // Done when the caller stops - end_of_stream, which is the
                                   // ordinary ending - or when the deadline set in LingeringClose()
                                   // fires. Either way the connection ends here.
                                   if (ec) {
                                       self->Close();
                                       return;
                                   }
                                   self->drainNext(drain);
                               });
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
