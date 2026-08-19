#pragma once

// C++ includes
#include <chrono>
#include <stdexcept>
#include <string>

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
     * @brief Minimal JSON-over-HTTP(S) client used to talk to the euclid server. Request and
     * response bodies are always JSON.
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
         * @return parsed response
         */
        [[nodiscard]]
        HttpResponse Post(const std::string &target, const std::string &action, const boost::json::value &body) const;

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

    private:

        /**
         * @brief Connects to _endpoint (HTTP or HTTPS, inferred from the endpoint scheme), sends
         * the request and returns the parsed response. Throws std::runtime_error on any
         * connection/transport failure.
         *
         * @param method HTTP method
         * @param target module target, sent as the "x-euclid-target" header
         * @param action module action, sent as the "x-euclid-action" header
         * @param body JSON request body, or nullptr for requests without a body
         * @return parsed response
         */
        [[nodiscard]]
        HttpResponse Send(HttpMethod method, const std::string &target, const std::string &action, const boost::json::value *body) const;

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