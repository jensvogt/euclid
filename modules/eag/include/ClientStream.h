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
     * @author jens.vogt\@opitz-consulting.com
     */
    class ClientStream {

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
         * @brief Reads one request from the caller.
         */
        void AsyncRead(boost::beast::flat_buffer &buffer,
                       boost::beast::http::request<boost::beast::http::string_body> &request,
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
         * @brief The caller's address, for the X-Forwarded-For header, or nothing if the
         * connection has already gone away.
         */
        [[nodiscard]]
        std::optional<boost::asio::ip::tcp::endpoint> RemoteEndpoint() const;

    private:

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
