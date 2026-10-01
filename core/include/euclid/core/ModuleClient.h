// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <algorithm>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

// Boost includes
#include <boost/beast/http.hpp>

namespace Euclid::Core {

    /**
     * @brief Calling one of euclid's modules directly, over the Unix socket it listens on.
     *
     * @par Why this is in core
     * Three things need it and only one of them is a transfer server. The FTP and SFTP servers call
     * ESM for every file operation; the manager fetches an application's artifact the same way
     * (see docs/worker-nodes.md §3.1); and a worker, when there is one, will do nothing else. It
     * lived in the transfer servers' shared library because they were the first to need it, which
     * is a reason for where it started rather than where it belongs.
     *
     * @par What is here and what is not
     * The transport only: a socket path in, a response out. Finding which socket a module is
     * listening on means reading the module repository, and that lives a layer above this one -
     * core does not depend on the database and should not start. Callers that have the repository
     * resolve the path and pass it; see Transfer::CallModule() for the version that does.
     *
     * @author jensvogt47\@gmail.com
     */
    namespace ModuleClient {

        /**
         * @brief Result of one call to a module over its Unix domain socket.
         */
        struct ModuleResponse {

            /**
             * @brief HTTP status returned by the module, or 0 if the call never got that far.
             */
            int status{};

            /**
             * @brief Response body, which for ESM's object actions is the object's raw bytes.
             */
            std::string body;

            /**
             * @brief Whether the call succeeded.
             */
            [[nodiscard]] bool ok() const { return status >= 200 && status < 300; }

            /**
             * @brief The status and, when there is one, what the module said about it - for logging
             * a call that failed.
             *
             * @par
             * A refusal explains itself in its body: the authorization gate answers 403 with the
             * permission it wanted or the grant it could not find, and every other module error
             * says what was wrong with the request. Logging the bare status throws all of that
             * away, which is how "status: 403" on every transfer call read for two weeks as a
             * missing grant when the requests were in fact asking for a permission that cannot
             * exist.
             *
             * @par
             * Truncated, because an object action's body is the object: a failed download would
             * otherwise put a megabyte of file into the log. A module's error is a sentence.
             */
            [[nodiscard]] std::string describe() const {

                constexpr std::size_t kMaxReason = 512;
                if (body.empty()) return std::to_string(status);

                auto reason = body.substr(0, kMaxReason);
                if (body.size() > kMaxReason) reason += "...";

                // Control characters mean the body is bytes rather than a message - an object, not
                // an explanation - and those belong nowhere near a log line.
                if (std::ranges::any_of(reason, [](const unsigned char c) { return c < 0x20 && c != '\t'; })) {
                    return std::to_string(status) + " (" + std::to_string(body.size()) + " bytes)";
                }
                return std::to_string(status) + ", reason: " + reason;
            }
        };

        /**
         * @brief Builds the request CallAt() puts on a module socket.
         *
         * @par
         * Separated so the wire contract can be asserted without a socket: a module is reached over
         * AF_UNIX, and Boost.Asio cannot bind one of those on Windows at all, so a test that needed
         * a listener would not run on the platform this was found on.
         *
         * @par
         * What is worth pinning is that both halves of the permission are sent. The authorization
         * gate requires "<x-euclid-target>:<x-euclid-action>", and a request missing the target
         * asks to be allowed ":list-objects" - which is not a permission the vocabulary holds, so
         * it is refused before any grant is read, by every role including one granted everything.
         */
        inline boost::beast::http::request<boost::beast::http::string_body>
        BuildRequest(const std::string &moduleName, const std::string &action, const std::string &token,
                     const std::vector<std::pair<std::string, std::string> > &headers, const std::string &body) {

            boost::beast::http::request<boost::beast::http::string_body> req{boost::beast::http::verb::post, "/", 11};

            req.set("x-euclid-target", moduleName);
            req.set("x-euclid-action", action);
            if (!token.empty()) req.set(boost::beast::http::field::authorization, "Bearer " + token);
            for (const auto &[name, value]: headers) req.set(name, value);
            req.body() = body;
            req.prepare_payload();

            return req;
        }

        /**
         * @brief Calls one action on one specific module instance.
         *
         * @param socketPath the instance's socket.
         * @param moduleName the module being called, for the x-euclid-target header. The
         * authorization gate builds the permission it requires out of that header and
         * x-euclid-action, so a request without it asks to be allowed ":list-objects", which is not
         * a permission any role can hold - and is refused with a 403 no grant can fix.
         * @param action value for the x-euclid-action header.
         * @param token bearer token to authenticate as.
         * @param headers additional request headers, e.g. x-euclid-bucket-ern.
         * @param body request body, raw.
         * @return the module's response, or a zero status if the instance could not be reached.
         */
        [[nodiscard]]
        ModuleResponse CallAt(const std::string &socketPath, const std::string &moduleName, const std::string &action,
                              const std::string &token, const std::vector<std::pair<std::string, std::string> > &headers,
                              const std::string &body);

    }// namespace ModuleClient

}// namespace Euclid::Core
