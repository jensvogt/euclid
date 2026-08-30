#pragma once

// C++ includes
#include <deque>
#include <memory>
#include <string>

// Boost includes
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

// Euclid includes
#include <euclid/manager/Controller.h>

namespace Euclid::main {

    /**
     * @brief What GatewayWsRegistry needs from a websocket session to deliver a pushed event to
     * it, without needing to know whether the underlying connection is plain TCP or TLS.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class IWsSession {
    public:
        virtual ~IWsSession() = default;

        /**
         * @brief Queues a frame (a response or a pushed event) for delivery, serialized with
         * every other write on this connection - safe to call from any thread.
         */
        virtual void PostFrame(std::string frame) = 0;

        /**
         * @brief Account this session authenticated for at handshake time - events are only
         * delivered to sessions whose accountId/region match the pushed event's.
         */
        [[nodiscard]] virtual const std::string &accountId() const = 0;

        /**
         * @brief Region this session authenticated for at handshake time.
         */
        [[nodiscard]] virtual const std::string &region() const = 0;
    };

    /**
     * @brief One persistent plaintext websocket connection to the gateway, used by clients (e.g.
     * Euclid-JDK) that want both request/response action calls and server-pushed events
     * multiplexed over a single connection, instead of one-shot HTTP.
     *
     * The gateway itself authenticates only once, at handshake time (the Authorization/
     * x-euclid-* headers on the HTTP Upgrade request are checked the same way
     * Core::HttpActionServer::Authenticate() checks them per-request over plain HTTP) as a
     * fast-fail routing gate - it does not re-run that check per frame. The original Authorization
     * header value is still cached and threaded through on every synthetic per-frame request
     * (see WsFrame::ToHttpRequest()), because each backend module independently re-authenticates
     * every request it receives to resolve its own caller identity, exactly as it does for a
     * request forwarded from the HTTP path. What *is* a deliberate departure from the rest of the
     * gateway (which re-authenticates every single HTTP request even on a keep-alive connection,
     * see GatewaySession/GatewayTlsSession in GatewayServer.cpp) is that the gateway's own
     * fast-fail gate isn't repeated per frame - it's the necessary tradeoff for a long-lived
     * multiplexed connection, but means a session whose credentials are revoked mid-connection
     * (e.g. token blacklisted) keeps passing the gateway's own gate until the connection closes,
     * even though each backend module would still independently reject an actually-expired token.
     *
     * Each inbound request frame is dispatched onto the owning io_context so concurrent in-flight
     * requests on one connection don't block each other or the read loop - see handleRequestFrame
     * in GatewayWsSession.cpp. Every outbound frame (a response or a pushed event, the latter
     * arriving from GatewayWsRegistry::Broadcast() on an unrelated thread) goes through one
     * PostFrame()-fed write queue, since Beast websocket writes aren't safely concurrent.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class GatewayWsSession final : public IWsSession, public std::enable_shared_from_this<GatewayWsSession> {
    public:
        GatewayWsSession(boost::beast::tcp_stream stream, boost::asio::io_context &ioc, ServiceController &ctrl,
                          std::string authorization, std::string accountId, std::string region, std::string ns);

        /**
         * @brief Completes the websocket handshake against the already-read HTTP upgrade
         * request, registers this session with GatewayWsRegistry for event delivery, and starts
         * the read loop.
         */
        void run(boost::beast::http::request<boost::beast::http::string_body> req);

        void PostFrame(std::string frame) override;
        [[nodiscard]] const std::string &accountId() const override { return _accountId; }
        [[nodiscard]] const std::string &region() const override { return _region; }

    private:
        void onAccept(const boost::beast::error_code &ec);
        void doRead();
        void onRead(const boost::beast::error_code &ec, std::size_t bytes);
        void handleRequestFrame(const std::string &text);
        void doWrite();

        boost::beast::websocket::stream<boost::beast::tcp_stream> _ws;
        boost::asio::io_context &_ioc;
        ServiceController &_ctrl;
        std::string _authorization;
        std::string _accountId;
        std::string _region;
        std::string _namespace;
        boost::beast::flat_buffer _buffer;
        std::deque<std::string> _writeQueue;
    };

    /**
     * @brief TLS-terminated counterpart of GatewayWsSession - identical behavior/protocol, see
     * its doc comment; only the underlying stream type differs.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class GatewayWsTlsSession final : public IWsSession, public std::enable_shared_from_this<GatewayWsTlsSession> {
    public:
        GatewayWsTlsSession(boost::beast::ssl_stream<boost::beast::tcp_stream> stream, boost::asio::io_context &ioc, ServiceController &ctrl,
                             std::string authorization, std::string accountId, std::string region, std::string ns);

        void run(boost::beast::http::request<boost::beast::http::string_body> req);

        void PostFrame(std::string frame) override;
        [[nodiscard]] const std::string &accountId() const override { return _accountId; }
        [[nodiscard]] const std::string &region() const override { return _region; }

    private:
        void onAccept(const boost::beast::error_code &ec);
        void doRead();
        void onRead(const boost::beast::error_code &ec, std::size_t bytes);
        void handleRequestFrame(const std::string &text);
        void doWrite();

        boost::beast::websocket::stream<boost::beast::ssl_stream<boost::beast::tcp_stream> > _ws;
        boost::asio::io_context &_ioc;
        ServiceController &_ctrl;
        std::string _authorization;
        std::string _accountId;
        std::string _region;
        std::string _namespace;
        boost::beast::flat_buffer _buffer;
        std::deque<std::string> _writeQueue;
    };

}// namespace Euclid::main
