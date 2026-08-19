//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <string>

namespace Euclid::Core {

    /**
     * @brief Password hashing utilities.
     *
     * Hashes are salted PBKDF2-HMAC-SHA256, encoded as
     * "pbkdf2-sha256$<iterations>$<salt-hex>$<hash-hex>".
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class PasswordUtils {
    public:

        /**
         * @brief Hashes a plaintext password with a freshly generated random salt.
         *
         * @param password plaintext password.
         * @return encoded hash string, safe to store.
         */
        static std::string Hash(const std::string &password);

        /**
         * @brief Verifies a plaintext password against a previously hashed value.
         *
         * @param password plaintext password to check.
         * @param hash encoded hash string, as produced by Hash().
         * @return true if the password matches.
         */
        static bool Verify(const std::string &password, const std::string &hash);
    };

}// namespace Euclid::Core
