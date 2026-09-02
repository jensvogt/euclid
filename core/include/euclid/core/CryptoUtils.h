//
// Created by vogje01 on 8/17/26.
//

#pragma once

// C++ includes
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Euclid::Core {

    /**
     * @brief An MD5 sum computed over data that is never all in memory - or never on disk - at once.
     *
     * @par
     * CryptoUtils::md5Sum() takes a string and md5SumFile() takes a path, which covers everything
     * that is one or the other. This covers what is neither: bytes that arrive in pieces and are
     * gone again once written, e.g. a multipart upload being assembled and encrypted in one pass,
     * where the plaintext the checksum has to describe exists only as it streams past.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class Md5Digest {
    public:

        /**
         * @brief Starts a new, empty digest.
         */
        Md5Digest();

        /**
         * @brief Adds the next piece of data to the digest.
         *
         * @param data bytes to hash; may be empty.
         */
        void update(std::string_view data) const;

        /**
         * @brief Finalizes the digest and returns it.
         *
         * @par
         * Finalizing consumes the underlying context - call this once, after the last update().
         *
         * @return MD5 sum of everything passed to update(), as a lowercase hex-encoded string.
         */
        [[nodiscard]]
        std::string hex() const;

    private:

        // EVP_MD_CTX, kept behind an incomplete type so this header doesn't drag OpenSSL into
        // everything that includes it.
        struct Context;

        std::shared_ptr<Context> _context;
    };

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
         * @param aad additional authenticated data - covered by the authentication tag but not
         * encrypted, and not part of the result, so the same value has to be supplied again to
         * decrypt. For binding a ciphertext to the context it belongs in: a caller that splits
         * data into separately encrypted pieces can put each piece's position in here, and a
         * piece moved, duplicated or dropped then fails to authenticate instead of decrypting
         * cleanly in the wrong place. Empty (the default) means no AAD.
         * @return a random 12-byte IV, the ciphertext, and the 16-byte GCM authentication tag,
         * concatenated as IV || ciphertext || tag. Both the IV and the tag are needed to decrypt,
         * so they travel alongside the ciphertext rather than being returned separately.
         */
        static std::string AesGcmEncrypt(const std::string &key, const std::string &plaintext, const std::string &aad = {});

        /**
         * @brief Decrypts data produced by AesGcmEncrypt().
         *
         * @param key raw AES key bytes - 16 bytes selects AES-128-GCM, 32 bytes selects
         * AES-256-GCM. Decode a stored key (e.g. Key::keyMaterial) with Base64Decode() first.
         * @param ciphertext IV || ciphertext || tag, as returned by AesGcmEncrypt().
         * @param aad the same additional authenticated data that was passed to AesGcmEncrypt();
         * authentication fails if it differs.
         * @return the decrypted plaintext.
         * @throws std::runtime_error if ciphertext is too short to contain an IV and tag, or if
         * authentication fails (wrong key or AAD, or the data was tampered with / truncated).
         */
        static std::string AesGcmDecrypt(const std::string &key, const std::string &ciphertext, const std::string &aad = {});

        /**
         * @brief Generates a random salt for DeriveKeyPbkdf2().
         *
         * @param length number of bytes to draw.
         * @return raw random bytes - base64-encode them before putting them in JSON.
         */
        static std::string GenerateSalt(std::size_t length);

        /**
         * @brief Derives an AES key from a passphrase, PBKDF2-HMAC-SHA256.
         *
         * For turning something a person typed into something AesGcmEncrypt() will take. Neither
         * the salt nor the iteration count is a secret - both are stored next to the ciphertext,
         * since decrypting needs the same ones - but the salt has to be fresh per archive, or two
         * archives protected by the same passphrase end up under the same key.
         *
         * @param passphrase the typed secret.
         * @param salt       raw salt bytes, as returned by GenerateSalt().
         * @param iterations PBKDF2 iteration count; costs an attacker what it costs you.
         * @param keyLength  bytes of key material to derive - 16 for AES-128, 32 for AES-256.
         * @return the derived key as raw bytes, ready for AesGcmEncrypt()/AesGcmDecrypt().
         */
        static std::string DeriveKeyPbkdf2(const std::string &passphrase, const std::string &salt, int iterations, std::size_t keyLength);

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
