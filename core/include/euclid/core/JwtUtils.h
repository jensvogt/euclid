//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <chrono>
#include <optional>
#include <string>

namespace Euclid::Core {

    /**
     * @brief HS256 JWT creation and verification.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class JwtUtils {
    public:

        /**
         * @brief Creates a signed HS256 JWT.
         *
         * @param subject value of the "sub" claim, e.g. the user ID.
         * @param secret HMAC signing secret.
         * @param ttl token lifetime, from now.
         * @return the encoded, signed token.
         */
        static std::string CreateToken(const std::string &subject, const std::string &secret, std::chrono::seconds ttl = std::chrono::hours(1));

        /**
         * @brief Verifies a token's signature and expiry.
         *
         * @param token encoded JWT.
         * @param secret HMAC signing secret used to create the token.
         * @return the "sub" claim if the token is valid, otherwise std::nullopt.
         */
        static std::optional<std::string> VerifyToken(const std::string &token, const std::string &secret);

        /**
         * @brief Checks whether a token would verify (correct signature/algorithm) if not for
         * having expired. Used to give a more specific error than VerifyToken()'s plain
         * std::nullopt, e.g. to tell an expired token apart from a garbled/forged one.
         *
         * @param token encoded JWT.
         * @param secret HMAC signing secret used to create the token.
         * @return true only if the token is otherwise valid but its "exp" claim has passed.
         */
        static bool IsTokenExpired(const std::string &token, const std::string &secret);
    };

}// namespace Euclid::Core
