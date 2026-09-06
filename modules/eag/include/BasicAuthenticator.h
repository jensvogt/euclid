//
// Created by vogje01 on 9/6/26.
//

#pragma once

// C++ includes
#include <chrono>
#include <map>
#include <mutex>
#include <string>

namespace Euclid::EAG {

    /**
     * @brief Checks HTTP Basic credentials against euclid's own user store.
     *
     * @par
     * Only the gateway does this, never a euclid module. A euclid password is stored as PBKDF2 at
     * 100,000 iterations, which takes tens of milliseconds on purpose: right for a login that
     * happens once, and quite wrong for something on the path of every request. Putting it behind
     * the gateway keeps that cost off ESM, EQS and the rest, where a bearer token or a signature
     * costs an HMAC and nothing more.
     *
     * @par
     * It also means the cost can be paid once rather than per request. A verified credential is
     * remembered for a short window, so a browser making a hundred calls hashes once and compares
     * ninety-nine times. What is remembered is the password's own hash, never the password.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class BasicAuthenticator {

    public:

        /**
         * @brief Constructs the authenticator.
         *
         * @param cacheSeconds how long a verified credential stays verified. Zero checks every
         * request, which is correct and expensive.
         */
        explicit BasicAuthenticator(long cacheSeconds);

        /**
         * @brief Verifies an Authorization header value.
         *
         * @par
         * Returns the userId behind the credential, or nothing if the header is absent, is not
         * Basic, does not decode, names no user, names a technical principal, or carries the wrong
         * password. The caller is told none of that - which of them it was is exactly what an
         * attacker probing for valid usernames wants to learn - so it goes to the log and the
         * answer is the same 401 either way.
         *
         * @param authorization the request's Authorization header, or empty if it had none.
         * @return the authenticated userId.
         */
        [[nodiscard]]
        std::optional<std::string> Verify(const std::string &authorization);

    private:

        /**
         * @brief One credential this process has already checked.
         */
        struct Entry {

            /**
             * @brief The user the credential belongs to.
             */
            std::string userId;

            /**
             * @brief When this stops being trusted and the password is hashed again.
             */
            std::chrono::steady_clock::time_point expiresAt;
        };

        /**
         * @brief How long a verified credential stays verified.
         */
        std::chrono::seconds _cacheFor;

        mutable std::mutex _mutex;

        /**
         * @brief Verified credentials, keyed by a hash of the header rather than by the header -
         * so a heap dump of a running gateway does not hand somebody a list of passwords.
         */
        std::map<std::string, Entry> _verified;
    };

}// namespace Euclid::EAG
