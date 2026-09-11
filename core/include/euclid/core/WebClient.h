// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/9/26.
//

#pragma once

// C++ includes
#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Euclid::Core {

    /**
     * @brief What a web request answered with.
     */
    struct WebResponse {

        /**
         * @brief HTTP status code, e.g. 200.
         */
        int status{0};

        /**
         * @brief The response body, whatever its content type.
         */
        std::string body;

        /**
         * @brief True for 2xx.
         */
        [[nodiscard]]
        bool IsSuccess() const { return status >= 200 && status < 300; }
    };

    /**
     * @brief How to make the request. Everything is optional; the defaults are a plain GET.
     */
    struct WebRequestOptions {

        /**
         * @brief HTTP method name, e.g. "GET" or "POST".
         */
        std::string method{"GET"};

        /**
         * @brief Request body, or empty for none.
         */
        std::string body;

        /**
         * @brief Content type of body; ignored when there is no body.
         */
        std::string contentType{"application/json"};

        /**
         * @brief Value of the Authorization header, or empty for none.
         */
        std::string authorization;

        /**
         * @brief Any further headers, by name and value.
         */
        std::vector<std::pair<std::string, std::string> > headers;

        /**
         * @brief PEM CA certificate trusted in addition to the system store, for a server behind a
         * private CA.
         */
        std::string caCertFile;

        /**
         * @brief How long the server has to answer before the request is given up on.
         */
        std::chrono::seconds timeout{10};
    };

    /**
     * @brief Raised when a request cannot be made or completed - a name that will not resolve, a
     * refused connection, a failed handshake, a server that stops answering.
     *
     * @par
     * A status code is not a failure here: a 401 is an answer, and the caller is the one that knows
     * what to make of it.
     */
    struct WebError final : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    /**
     * @brief Makes one HTTP or HTTPS request to somebody else's server, and waits for the answer.
     *
     * @par
     * Euclid talks to its own modules over Unix sockets and needs none of this; what needs it is
     * the handful of places where euclid is the client of an outside service - an identity
     * provider's token endpoint, its key set, its API. Written once here rather than in each of
     * them, because the parts worth getting right (certificate verification, server name
     * indication, a timeout on every step) are the same every time.
     *
     * @par
     * Asynchronous underneath and synchronous to the caller: beast applies its timeouts to
     * asynchronous operations only, and a server that has gone away must not be able to park the
     * thread that is waiting on it forever.
     *
     * @param url absolute URL, http or https.
     * @param options what to send and how long to wait.
     * @return the status and body.
     * @throws WebError if the request could not be completed.
     */
    WebResponse WebFetch(const std::string &url, const WebRequestOptions &options = {});

}// namespace Euclid::Core
