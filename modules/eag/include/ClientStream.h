// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/7/26.
//

#pragma once

// C++ includes
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <variant>

// Boost includes
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

namespace Euclid::EAG {

    /**
     * @brief One caller's connection, whether it arrived over HTTP or HTTPS.
     *
     * @par
     * The two differ in the stream type all the way down - beast::tcp_stream against
     * beast::ssl_stream<beast::tcp_stream> - and everything between accepting a connection and
     * answering it would otherwise have to exist twice, or be a template whose instantiations are
     * threaded through a dozen asynchronous continuations. Neither is worth it for what is really
     * one decision made once per listener, so the difference is held here and nowhere else: the
     * proxy reads a request, forwards it and writes a response without knowing which it has.
     *
     * @par
     * Only the operations the gateway actually performs on a client connection are exposed, and
     * only for the one message type it reads and the one it writes. That is what keeps this a
     * variant with a handful of visits rather than a general-purpose stream wrapper.
     *
     * @author jensvogt47\@gmail.com
     */
    class ClientStream : public std::enable_shared_from_this<ClientStream> {

    public:

        /**
         * @brief Takes over a connection that arrived on a plain HTTP listener.
         */
        explicit ClientStream(boost::asio::ip::tcp::socket socket);

        /**
         * @brief Takes over a connection that arrived on an HTTPS listener.
         *
         * @param socket the accepted socket.
         * @param context the listener's server context, holding its certificate and key. Held for
         * the life of the connection, because OpenSSL keeps referring to it after the handshake.
         */
        ClientStream(boost::asio::ip::tcp::socket socket, std::shared_ptr<boost::asio::ssl::context> context);

        ClientStream(const ClientStream &) = delete;
        ClientStream &operator=(const ClientStream &) = delete;

        /**
         * @brief Whether this connection is TLS, which is what the forwarded X-Forwarded-Proto
         * header has to say.
         */
        [[nodiscard]]
        bool IsTls() const;

        /**
         * @brief Sets how long the next operation on this connection may take.
         */
        void ExpiresAfter(std::chrono::seconds timeout);

        /**
         * @brief Completes the TLS handshake, or calls back immediately with no error for a plain
         * connection.
         *
         * @par
         * Called unconditionally, so that serve() does not branch: a plain connection is simply
         * one whose handshake is already done.
         */
        void Handshake(std::function<void(boost::beast::error_code)> done);

        /**
         * @brief A request whose headers have been read and whose body has not.
         *
         * @par
         * Every request starts as one of these, because what the gateway does with a body depends
         * on the route, and the route is only known once the path and method have arrived. A
         * proxied request then becomes a BodyReader and an uploaded one a StreamReader - the two
         * ways of reading the same remaining bytes.
         */
        using HeaderReader = boost::beast::http::request_parser<boost::beast::http::empty_body>;

        /**
         * @brief The rest of a request, read into memory in one go.
         */
        using BodyReader = boost::beast::http::request_parser<boost::beast::http::string_body>;

        /**
         * @brief The rest of a request, read a chunk at a time into a buffer the caller supplies.
         */
        using StreamReader = boost::beast::http::request_parser<boost::beast::http::buffer_body>;

        /**
         * @brief Reads one request's headers, leaving its body on the connection.
         */
        void AsyncReadHeader(boost::beast::flat_buffer &buffer, HeaderReader &reader,
                             std::function<void(boost::beast::error_code)> done);

        /**
         * @brief Reads the rest of a request whose headers have already been read.
         */
        void AsyncRead(boost::beast::flat_buffer &buffer, BodyReader &reader,
                       std::function<void(boost::beast::error_code)> done);

        /**
         * @brief Reads as much of the body as is available into the buffer the reader points at.
         *
         * @par
         * Called repeatedly until the reader says it is done. Completes with
         * http::error::need_buffer when it has filled the buffer and there is more to come, which
         * is not a failure - it is the reader asking to be given somewhere to put the next piece.
         */
        void AsyncReadSome(boost::beast::flat_buffer &buffer, StreamReader &reader,
                           std::function<void(boost::beast::error_code)> done);

        /**
         * @brief Writes one response back to the caller.
         */
        void AsyncWrite(boost::beast::http::response<boost::beast::http::string_body> &response,
                        std::function<void(boost::beast::error_code)> done);

        /**
         * @brief Ends the connection.
         *
         * @par
         * A TLS connection is closed at the socket rather than shut down at the TLS layer: a
         * close_notify exchange waits for a peer that has already stopped listening, on a worker
         * thread shared with every other connection this gateway is serving - the same hazard the
         * euclid gateway's own sessions describe. One exchange per connection is what this serves,
         * and the response's Content-Length has already told the caller where the body ends.
         */
        void Close();

        /**
         * @brief Reads and throws away whatever the caller is still sending, then ends the
         * connection.
         *
         * @par Why a response is not the end of it
         * A request is refused before its body has been read whenever the refusal can be decided
         * from the headers - a body larger than the route takes, a content type it does not, a
         * part ESM would not store. The response goes out while the caller is still writing, which
         * is the point: there is no reason to carry twenty gigabytes to reject it.
         *
         * @par
         * Closing there loses the refusal. The caller goes on writing into a socket nobody is
         * reading, the receive buffer fills, and a connection closed with unread data in it is
         * reset rather than finished - and a reset throws away what the socket had already
         * received, including the response that explains everything. What the caller sees is not
         * "413 Payload Too Large" but a connection dropped mid-upload, with nothing to act on.
         *
         * @par
         * So the send side closes, the receive side keeps reading until the caller stops, and the
         * connection ends in order. This is what nginx calls a lingering close, and it is bounded
         * the same way: whichever comes first of the caller finishing, a few seconds passing, or
         * enough bytes read to say they are not stopping. A caller who keeps writing regardless
         * gets the reset they were always going to get, and the gateway is not still holding a
         * socket for them.
         */
        void LingeringClose();

        /**
         * @brief The caller's address, for the X-Forwarded-For header, or nothing if the
         * connection has already gone away.
         */
        [[nodiscard]]
        std::optional<boost::asio::ip::tcp::endpoint> RemoteEndpoint() const;

    private:

        /**
         * @brief What a lingering close is carrying: somewhere to put the bytes being discarded,
         * and how many there have been.
         */
        struct Drain;

        /**
         * @brief Reads one more helping of what the caller is still sending, and arms itself again
         * until there is nothing more, the deadline passes, or enough has been read.
         *
         * @par
         * Keeps the stream alive itself, through shared_from_this() in the handler: whoever asked
         * for the close has answered its caller and let go of its own reference by the time the
         * first read completes.
         */
        void drainNext(const std::shared_ptr<Drain> &drain);

        /**
         * @brief The connection itself: plain or TLS, decided when it was accepted and never
         * again.
         */
        std::variant<boost::beast::tcp_stream, boost::beast::ssl_stream<boost::beast::tcp_stream> > _stream;

        /**
         * @brief The listener's TLS context, kept alive alongside the stream that uses it. Null
         * for a plain connection.
         */
        std::shared_ptr<boost::asio::ssl::context> _context;
    };

}// namespace Euclid::EAG
