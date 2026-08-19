#pragma once

// C++ includes
#include <iostream>

// Boost includes
#include <boost/beast/http.hpp>

namespace Euclid::Core {

    namespace http = boost::beast::http;

    /**
     * @brief Dumps an HTTP request (headers and body) to stdout.
     */
    void dumpRequest(const http::request<http::string_body> &req);

    /**
     * @brief Dumps an HTTP response (headers and body) to stdout.
     */
    void dumpResponse(const http::response<http::string_body> &res);

} // namespace Euclid::main
