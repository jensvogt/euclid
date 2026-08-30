#pragma once

// C++ includes
#include <optional>
#include <string>
#include <string_view>

// Boost includes
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

namespace Euclid::Core {

    /**
     * @brief Wire protocol for the gateway's websocket transport: JSON envelopes multiplexing
     * request/response action calls and server-pushed events over one persistent connection,
     * alongside the gateway's existing one-shot HTTP path.
     *
     * Pure, socket-free (de)serialization only - main's GatewayWsSession/GatewayWsTlsSession own
     * the actual websocket::stream and call into this for every frame they send/receive, so the
     * framing logic itself stays unit-testable without a real connection (see
     * tests/GatewayWsFrameTest.cpp).
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class WsFrame {
    public:

        /**
         * @brief A parsed inbound "request" frame - see ParseRequest().
         */
        struct Request {
            /**
             * @brief Client-chosen correlation id, echoed back verbatim on the matching
             * "response"/"error" frame so a client can multiplex several concurrent in-flight
             * requests over one connection. Unlike x-euclid-request-id (server-generated, per
             * HTTP response only), this one genuinely originates from the client.
             */
            std::string id;

            /**
             * @brief Target service, e.g. "ekm" - maps 1:1 onto the x-euclid-target header used
             * by the HTTP path.
             */
            std::string target;

            /**
             * @brief Action name, e.g. "create-key" - maps 1:1 onto the x-euclid-action header.
             */
            std::string action;

            /**
             * @brief Request payload, passed through as the backend HTTP request's JSON body.
             */
            boost::json::value body;
        };

        /**
         * @brief Parses one inbound websocket text frame as a client request.
         *
         * @param text  raw frame payload
         * @param error set to a human-readable message when parsing fails
         * @return the parsed request, or std::nullopt if text isn't a well-formed
         * {"type":"request",...} frame (malformed JSON, wrong/missing "type", or missing
         * id/target/action).
         */
        [[nodiscard]]
        static std::optional<Request> ParseRequest(const std::string &text, std::string &error);

        /**
         * @brief Builds the synthetic HTTP request the existing gateway routing path
         * (ServiceController::acquireInstance() + forwardToService()) already understands, from
         * a parsed websocket request frame plus the caller's identity cached on the websocket
         * session at handshake time.
         *
         * Carries the same x-euclid-target/x-euclid-action/x-euclid-account-id/-region/
         * -namespace headers a one-shot HTTP call would, plus the original Authorization header
         * from the upgrade request - the gateway's own handshake-time check
         * (Core::HttpActionServer::Authenticate()) is only a fast-fail routing-layer gate; every
         * module independently re-authenticates each request it receives to resolve its own
         * caller identity (e.g. EkmServer::handleCreateKey() needs auth.user->accountId), exactly
         * as it does for a request forwarded from the HTTP path - so the same original bearer
         * token/SigV4 signature has to be threaded through here too, not dropped.
         */
        [[nodiscard]]
        static boost::beast::http::request<boost::beast::http::string_body>
        ToHttpRequest(const Request &request, const std::string &authorization, const std::string &accountId, const std::string &region, const std::string &ns);

        /**
         * @brief Builds the outbound "response" frame for a backend HTTP response, echoing the
         * request frame's client-supplied id.
         */
        [[nodiscard]]
        static std::string BuildResponseFrame(const std::string &id, const boost::beast::http::response<boost::beast::http::string_body> &res);

        /**
         * @brief Builds a protocol-level "error" frame - malformed frame, unknown target,
         * unavailable backend, etc. - as opposed to an application-level error, which arrives as
         * a non-2xx "response" frame instead.
         *
         * @param id echoed client-supplied correlation id, or empty if the frame that caused the
         * error couldn't even be parsed far enough to have one.
         */
        [[nodiscard]]
        static std::string BuildErrorFrame(const std::string &id, unsigned status, std::string_view message);

        /**
         * @brief Builds an unsolicited "event" frame, pushed by Core::EventPusher via the
         * gateway's event-ingest listener to every websocket session scoped to accountId/region.
         */
        [[nodiscard]]
        static std::string BuildEventFrame(const std::string &topic, const std::string &accountId, const std::string &region, const boost::json::object &body);
    };

}// namespace Euclid::Core
