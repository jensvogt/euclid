// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/20/26.
//

#pragma once

// C++ includes
#include <string>
#include <utility>
#include <vector>

// Boost includes
#include <boost/asio/ssl.hpp>

namespace Euclid::EAG {

    /**
     * @brief Who a call to a euclid module is made as.
     *
     * @par
     * Never the gateway itself. An upload writes into somebody's bucket, and which bucket they may
     * write into is a question about them - so the calls that do the writing carry their
     * credential and are authorized by ESM exactly as the same calls from the CLI would be. The
     * gateway having verified the caller is not the same as the gateway being allowed to act.
     *
     * @par
     * Which of the two fields is set depends on how the caller authenticated, not on anything
     * chosen here - see ProxyServer::uploadCredential().
     */
    struct ModuleCredential {

        /**
         * @brief A euclid session token, presented as "Authorization: Bearer".
         *
         * @par
         * What a caller who logged in already has, and what the gateway obtains on behalf of one
         * who presented HTTP Basic - by logging in as them, with the password they just sent,
         * rather than by minting anything.
         */
        std::string bearerToken;

        /**
         * @brief An access key the call is signed with, RFC 9421, when there is no token.
         *
         * @par
         * For a caller who signs their requests rather than holding a session. The gateway can
         * resolve the secret because it already must: verifying the signature they arrived with is
         * the same lookup. Signing onward calls with it therefore gives the gateway nothing it did
         * not have, and gives ESM the real caller to authorize.
         */
        std::string accessKeyId;
        std::string secretAccessKey;

        [[nodiscard]]
        bool IsPresent() const { return !bearerToken.empty() || !accessKeyId.empty(); }
    };

    /**
     * @brief One call to a euclid module, addressed the way every euclid call is.
     */
    struct ModuleCallOptions {

        /**
         * @brief The module, e.g. "esm".
         */
        std::string target;

        /**
         * @brief The action, e.g. "create-upload".
         */
        std::string action;

        /**
         * @brief The request body.
         */
        std::string body;

        /**
         * @brief Content type of the body. JSON for everything but an upload part, which is raw
         * bytes.
         */
        std::string contentType{"application/json"};

        /**
         * @brief The scope the call acts in, which the caller of an upload route never sent.
         */
        std::string region;
        std::string accountId;
        std::string userId;
        std::string nameSpace;

        /**
         * @brief Anything else the action needs, e.g. x-euclid-upload-id.
         */
        std::vector<std::pair<std::string, std::string> > headers;
    };

    /**
     * @brief What a module answered.
     */
    struct ModuleResponse {
        int status{0};
        std::string body;

        [[nodiscard]]
        bool IsSuccess() const { return status >= 200 && status < 300; }
    };

    /**
     * @brief Calls one euclid module through euclid's own gateway, and waits for the answer.
     *
     * @par
     * Through the gateway rather than at the module, for the reason a module route is proxied
     * there too: a module listens on a Unix domain socket and has no address anything outside its
     * own process tree could reach, and the gateway is what turns a target and an action into a
     * call on one.
     *
     * @par
     * Core::WebFetch does almost this and is not used, for one reason: it builds the request
     * itself, so there is no request for HttpSignature::Sign() to sign. A caller who authenticates
     * by signature would have to be downgraded to some other credential to go through it, and
     * downgrading a credential is exactly the kind of quiet weakening this is trying to avoid.
     *
     * @par
     * Synchronous, and therefore occupying the thread that calls it. Upload parts are sent one at a
     * time from the gateway's own worker threads, so an upload in flight holds one of them - which
     * is the backpressure that keeps the gateway from reading faster than ESM can store, and also
     * the reason a listener expecting many concurrent uploads wants more than the default eight.
     *
     * @param port       euclid's gateway port.
     * @param tls        whether it speaks TLS.
     * @param context    the client TLS context, used only when tls is set.
     * @param credential who the call is made as.
     * @param options    what to call and with what.
     * @return the module's answer, or a status of 0 with the reason in body if it could not be
     * reached at all.
     */
    [[nodiscard]]
    ModuleResponse CallModule(unsigned short port, bool tls, boost::asio::ssl::context &context,
                              const ModuleCredential &credential, const ModuleCallOptions &options);

}// namespace Euclid::EAG
