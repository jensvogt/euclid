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
         * @brief Generates an Euclid-style access key ID.
         *
         * @return "AKIA" followed by 16 random uppercase-alphanumeric characters.
         */
        static std::string GenerateAccessKeyId();

        /**
         * @brief Generates an Euclid-style secret access key.
         *
         * @return 40 cryptographically random bytes, base64-encoded.
         */
        static std::string GenerateSecretAccessKey();

        /**
         * @brief Generates a random 128-bit AES key.
         *
         * @return 16 cryptographically random bytes, base64-encoded.
         */
        static std::string GenerateAes128Key();

        /**
         * @brief Generates a random 256-bit AES key.
         *
         * @return 32 cryptographically random bytes, base64-encoded.
         */
        static std::string GenerateAes256Key();

        /**
         * @brief Encrypts data with AES-GCM.
         *
         * @param key raw AES key bytes - 16 bytes selects AES-128-GCM, 32 bytes selects
         * AES-256-GCM. Decode a stored key (e.g. Key::keyMaterial) with Base64Decode() first.
         * @param plaintext data to encrypt.
         * @return a random 12-byte IV, the ciphertext, and the 16-byte GCM authentication tag,
         * concatenated as IV || ciphertext || tag. Both the IV and the tag are needed to decrypt,
         * so they travel alongside the ciphertext rather than being returned separately.
         */
        static std::string AesGcmEncrypt(const std::string &key, const std::string &plaintext);

        /**
         * @brief Decrypts data produced by AesGcmEncrypt().
         *
         * @param key raw AES key bytes - 16 bytes selects AES-128-GCM, 32 bytes selects
         * AES-256-GCM. Decode a stored key (e.g. Key::keyMaterial) with Base64Decode() first.
         * @param ciphertext IV || ciphertext || tag, as returned by AesGcmEncrypt().
         * @return the decrypted plaintext.
         * @throws std::runtime_error if ciphertext is too short to contain an IV and tag, or if
         * authentication fails (wrong key, or the data was tampered with / truncated).
         */
        static std::string AesGcmDecrypt(const std::string &key, const std::string &ciphertext);

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

        /**
         * @brief Base64-encodes arbitrary bytes, e.g. for embedding binary payloads in a JSON request/response body.
         *
         * @param data raw bytes to encode.
         * @return base64-encoded string.
         */
        static std::string Base64Encode(const std::string &data);

        /**
         * @brief Decodes a base64-encoded string back to raw bytes.
         *
         * @param data base64-encoded input, as produced by Base64Encode().
         * @return decoded raw bytes.
         */
        static std::string Base64Decode(const std::string &data);
    };

}// namespace Euclid::Core
