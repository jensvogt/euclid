#pragma once

// C++ includes
#include <iostream>
#include <map>
#include <string>
#include <string_view>

// Boost includes
#include <boost/beast/http.hpp>

namespace Euclid::Core {

    namespace http = boost::beast::http;

    /**
     * @brief Splits the query string off a request target and percent-decodes both halves of each
     * parameter.
     *
     * @par
     * Euclid addresses its modules by header rather than by URL, so almost nothing here has a
     * query string to read. An OIDC redirect does - the provider chooses the URL and puts its code
     * and state on it - and both ends of that flow (the eam module and the CLI's loopback
     * listener) have to read the same thing out of it.
     *
     * @par
     * A malformed percent escape is passed through as the literal text it is, rather than
     * rejected: what the parameters mean is settled by a signature check further on, and failing
     * a login over an escape nobody wrote deliberately would only hide the real reason.
     *
     * @param target request target, with or without a query string, e.g. "/eam/oidc/callback?code=x&state=y".
     * @return the parameters, empty if there is no query string.
     */
    std::map<std::string, std::string> ParseQueryParameters(std::string_view target);

    /**
     * @brief Dumps an HTTP request (headers and body) to stdout.
     */
    void dumpRequest(const http::request<http::string_body> &req);

    /**
     * @brief Dumps an HTTP response (headers and body) to stdout.
     */
    void dumpResponse(const http::response<http::string_body> &res);

} // namespace Euclid::main
