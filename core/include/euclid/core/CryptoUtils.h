//
// Created by vogje01 on 8/17/26.
//

#pragma once

// C++ includes
#include <string>
#include <vector>

namespace Euclid::Core {

    /**
     * @brief Cryptographic hashing utilities.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class CryptoUtils {
    public:

        /**
         * @brief Calculates the MD5 sum of a string.
         *
         * @param str input string.
         * @return MD5 sum as a lowercase hex-encoded string.
         */
        static std::string md5Sum(const std::string &str);

        /**
         * @brief Calculates the MD5 sum of a file.
         *
         * @param filePath absolute path of the file.
         * @return MD5 sum as a lowercase hex-encoded string.
         */
        static std::string md5SumFile(const std::string &filePath);

        /**
         * @brief Generates an AWS-style access key ID.
         *
         * @return "AKIA" followed by 16 random uppercase-alphanumeric characters.
         */
        static std::string GenerateAccessKeyId();

        /**
         * @brief Generates an AWS-style secret access key.
         *
         * @return 40 cryptographically random bytes, base64-encoded.
         */
        static std::string GenerateSecretAccessKey();

        /**
         * @brief Calculates the SHA-256 sum of a string.
         *
         * @param str input string.
         * @return SHA-256 sum as a lowercase hex-encoded string.
         */
        static std::string sha256Hex(const std::string &str);

        /**
         * @brief Computes an HMAC-SHA256 over data, keyed with key.
         *
         * Used to build the SigV4 signing-key derivation chain (Core::SigV4), which needs the
         * raw MAC bytes rather than a hex string to feed into the next HMAC step.
         *
         * @param key  HMAC key (raw bytes, not hex/base64-encoded).
         * @param data data to authenticate.
         * @return the 32-byte MAC, as raw bytes.
         */
        static std::vector<unsigned char> hmacSha256(const std::vector<unsigned char> &key, const std::string &data);
    };

}// namespace Euclid::Core
