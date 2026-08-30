#pragma once

// C++ includes
#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Boost includes
#include <boost/json.hpp>
#include <boost/url.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

// Euclid includes
#include <euclid/cli/credentials/Credentials.h>
#include <euclid/core/Configuration.h>

namespace Euclid::CLI {

    /**
     * @brief HTTP method of a HttpClient request.
     */
    enum class HttpMethod {
        GET,
        POST,
        PUT,
        DEL
    };

    /**
     * @brief Result of an HTTP request: status code plus the parsed JSON response body.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    struct HttpResponse {

        /**
         * @brief HTTP status code, e.g. 200
         */
        int statusCode = 0;

        /**
         * @brief Parsed JSON response body, or null if the response had no body.
         */
        boost::json::value body;

        /**
         * @brief True for 2xx status codes.
         */
        [[nodiscard]]
        bool IsSuccess() const;
    };

    /**
     * @brief Result of PostForBinary(): status code plus the raw (non-JSON) response body.
     *
     * The mirror image of HttpResponse - used for the one action (storage's "download-part")
     * whose *response* is a large binary payload rather than JSON, the same way PostBinary()'s
     * request is for "upload-part".
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    struct BinaryHttpResponse {

        /**
         * @brief HTTP status code, e.g. 200
         */
        int statusCode = 0;

        /**
         * @brief Raw response body bytes on success; empty on failure (see errorBody instead).
         */
        std::string data;

        /**
         * @brief Parsed JSON error body, populated only when the request failed - a successful
         * response's body is raw bytes, not JSON, so there's nothing to parse into this.
         */
        boost::json::value errorBody;

        /**
         * @brief True for 2xx status codes.
         */
        [[nodiscard]]
        bool IsSuccess() const;
    };

    /**
     * @brief Minimal HTTP(S) client used to talk to the euclid server. Bodies are JSON except for
     * PostBinary(), used by the one action (storage's "upload-part") that trades the JSON
     * convention for transfer speed.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class HttpClient {

    public:

        /**
         * @brief Constructs the client.
         *
         * @param endpoint    Euclid server endpoint, e.g. "http://localhost:5566"
         * @param authentication if non-empty, sent as "Authorization: Bearer <bearerToken>" on every request
         * @param caCertPath if non-empty, path to a PEM CA certificate trusted in addition to the
         * system trust store, e.g. for self-signed development certificates
         */
        explicit HttpClient(std::string endpoint, Credentials::Entry authentication, std::string caCertPath = {});

        /**
         * @brief Sends a GET request. The request path is always "/" - routing is done via the
         * "x-euclid-target" and "x-euclid-action" headers.
         *
         * @param target module target, e.g. "access" (sent as the "x-euclid-target" header)
         * @param action module action, e.g. "login" (sent as the "x-euclid-action" header)
         * @return parsed response
         */
        [[nodiscard]]
        HttpResponse Get(const std::string &target, const std::string &action) const;

        /**
         * @brief Sends a POST request with a JSON body. The request path is always "/" - routing
         * is done via the "x-euclid-target" and "x-euclid-action" headers.
         *
         * @param target module target, e.g. "access" (sent as the "x-euclid-target" header)
         * @param action module action, e.g. "login" (sent as the "x-euclid-action" header)
         * @param body JSON request body
         * @param extraHeaders additional headers to set on the request, e.g. a declared concurrency hint
         * @return parsed response
         */
        [[nodiscard]]
        HttpResponse Post(const std::string &target, const std::string &action, const boost::json::value &body, const std::vector<std::pair<std::string, std::string> > &extraHeaders = {}) const;

        /**
         * @brief Sends a PUT request with a JSON body. The request path is always "/" - routing
         * is done via the "x-euclid-target" and "x-euclid-action" headers.
         *
         * @param target module target, e.g. "access" (sent as the "x-euclid-target" header)
         * @param action module action, e.g. "login" (sent as the "x-euclid-action" header)
         * @param body JSON request body
         * @return parsed response
         */
        [[nodiscard]]
        HttpResponse Put(const std::string &target, const std::string &action, const boost::json::value &body) const;

        /**
         * @brief Sends a DELETE request. The request path is always "/" - routing is done via the
         * "x-euclid-target" and "x-euclid-action" headers.
         *
         * @param target module target, e.g. "access" (sent as the "x-euclid-target" header)
         * @param action module action, e.g. "login" (sent as the "x-euclid-action" header)
         * @return parsed response
         */
        [[nodiscard]]
        HttpResponse Delete(const std::string &target, const std::string &action) const;

        /**
         * @brief Sends a POST request with a raw binary body instead of JSON, for actions where
         * base64-in-JSON overhead isn't worth paying (see storage's "upload-part": internal-only,
         * high-volume, and every byte doubles as network + CPU cost). The request path is always
         * "/" - routing is done via the "x-euclid-target" and "x-euclid-action" headers; any
         * request-specific metadata that would normally be a JSON field must travel in extraHeaders
         * instead, since the body is opaque bytes.
         *
         * @param target module target, e.g. "storage" (sent as the "x-euclid-target" header)
         * @param action module action, e.g. "upload-part" (sent as the "x-euclid-action" header)
         * @param extraHeaders additional headers to set on the request, e.g. upload ID/part number
         * @param data raw bytes to send as the request body ("application/octet-stream")
         * @return parsed response (the response body itself is still JSON)
         */
        [[nodiscard]]
        HttpResponse PostBinary(const std::string &target, const std::string &action, const std::vector<std::pair<std::string, std::string> > &extraHeaders, const std::string &data) const;

        /**
         * @brief Sends a POST request whose *response* is a raw binary body instead of JSON - the
         * mirror image of PostBinary(), for actions where the response (rather than the request)
         * is the large payload (see storage's "download-part"). Request metadata that would
         * normally be a JSON field travels in extraHeaders instead, and there's no request body.
         *
         * @param target module target, e.g. "storage" (sent as the "x-euclid-target" header)
         * @param action module action, e.g. "download-part" (sent as the "x-euclid-action" header)
         * @param extraHeaders additional headers to set on the request, e.g. download ID/part number
         * @return status code plus raw response bytes on success, or a parsed JSON error body on failure
         */
        [[nodiscard]]
        BinaryHttpResponse PostForBinary(const std::string &target, const std::string &action, const std::vector<std::pair<std::string, std::string> > &extraHeaders) const;

        /**
         * @brief Sends a POST request with a raw binary body and expects a raw binary response -
         * the combination of PostBinary() and PostForBinary(), for actions where both the request
         * and the response are opaque bytes (see EKM's "encrypt": plaintext in, ciphertext out).
         * The request path is always "/" - routing is done via the "x-euclid-target" and
         * "x-euclid-action" headers; any request-specific metadata must travel in extraHeaders.
         *
         * @param target module target, e.g. "ekm" (sent as the "x-euclid-target" header)
         * @param action module action, e.g. "encrypt" (sent as the "x-euclid-action" header)
         * @param extraHeaders additional headers to set on the request, e.g. the key ID
         * @param data raw bytes to send as the request body ("application/octet-stream")
         * @return status code plus raw response bytes on success, or a parsed JSON error body on failure
         */
        [[nodiscard]]
        BinaryHttpResponse PostBinaryForBinary(const std::string &target, const std::string &action, const std::vector<std::pair<std::string, std::string> > &extraHeaders, const std::string &data) const;

    private:

        /**
         * @brief Connects to _endpoint (HTTP or HTTPS, inferred from the endpoint scheme), sends
         * the request and returns the raw response. Throws std::runtime_error on any
         * connection/transport failure.
         *
         * @param method HTTP method
         * @param target module target, sent as the "x-euclid-target" header
         * @param action module action, sent as the "x-euclid-action" header
         * @param body JSON request body, or nullptr for requests without a body
         * @param extraHeaders additional headers to set on the request
         * @return parsed response
         */
        [[nodiscard]]
        HttpResponse Send(HttpMethod method, const std::string &target, const std::string &action, const boost::json::value *body, const std::vector<std::pair<std::string, std::string> > &extraHeaders = {}) const;

        /**
         * @brief Resolves _endpoint's host/port, sends a pre-built request over a plain or TLS
         * connection depending on scheme, and returns the raw response, unparsed. Shared by
         * Send(), PostBinary() and PostForBinary() so connection handling (timeouts, TLS setup,
         * error wrapping) lives in one place regardless of how the request body was built or how
         * the response body needs to be interpreted.
         *
         * @param target module target, used only for the error message on failure
         * @param action module action, used only for the error message on failure
         * @param request fully-built request ready to send
         * @return raw response
         */
        [[nodiscard]]
        boost::beast::http::response<boost::beast::http::string_body>
        Transmit(const std::string &target, const std::string &action, const boost::beast::http::request<boost::beast::http::string_body> &request) const;

        /**
         * @brief Euclid server endpoint, e.g. "http://localhost:5566"
         */
        std::string _endpoint;

        /**
         * @brief Authentication including the bearer token sent with every request, or empty for none.
         */
        Credentials::Entry _authentication;

        /**
         * @brief Path to an additional PEM CA certificate to trust, or empty to use only the
         * system trust store.
         */
        std::string _caCertPath;
    };

}