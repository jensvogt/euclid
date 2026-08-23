// C++ includes
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

// OpenSSL includes
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

// Euclid includes
#include <euclid/core/CryptoUtils.h>

namespace Euclid::Core {

    namespace {

        constexpr std::size_t kReadBufferSize = 1 << 16;

        std::string toHex(const unsigned char *digest, const unsigned int digestLength) {
            std::ostringstream oss;
            oss << std::hex << std::setfill('0');
            for (unsigned int i = 0; i < digestLength; ++i) oss << std::setw(2) << static_cast<int>(digest[i]);
            return oss.str();
        }

        // Draws random bytes from OpenSSL's CSPRNG. Throws rather than silently falling back to a
        // weaker source, since callers use this for access-key/secret material.
        std::vector<unsigned char> randomBytes(const std::size_t count) {
            std::vector<unsigned char> buf(count);
            if (RAND_bytes(buf.data(), static_cast<int>(count)) != 1) {
                throw std::runtime_error("Failed to generate random bytes");
            }
            return buf;
        }

    }// namespace

    std::string CryptoUtils::md5Sum(const std::string &str) {

        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int digestLength = 0;

        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(ctx, EVP_md5(), nullptr);
        EVP_DigestUpdate(ctx, str.data(), str.size());
        EVP_DigestFinal_ex(ctx, digest, &digestLength);
        EVP_MD_CTX_free(ctx);

        return toHex(digest, digestLength);
    }

    std::string CryptoUtils::md5SumFile(const std::string &filePath) {

        std::ifstream file(filePath, std::ios::in | std::ios::binary);
        if (!file.is_open()) throw std::runtime_error("Could not open file: " + filePath);

        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int digestLength = 0;

        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(ctx, EVP_md5(), nullptr);

        std::vector<char> buffer(kReadBufferSize);
        while (file.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || file.gcount() > 0) {
            EVP_DigestUpdate(ctx, buffer.data(), static_cast<std::size_t>(file.gcount()));
        }

        EVP_DigestFinal_ex(ctx, digest, &digestLength);
        EVP_MD_CTX_free(ctx);

        return toHex(digest, digestLength);
    }

    std::string CryptoUtils::GenerateAccessKeyId() {
        static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        constexpr std::size_t kIdChars = 16;

        const auto random = randomBytes(kIdChars);
        std::string id = "AKIA";
        id.reserve(id.size() + kIdChars);
        for (const unsigned char b: random) id += kAlphabet[b % (sizeof(kAlphabet) - 1)];
        return id;
    }

    std::string CryptoUtils::GenerateSecretAccessKey() {
        constexpr std::size_t kSecretBytes = 40;

        const auto random = randomBytes(kSecretBytes);

        std::string encoded(4 * ((kSecretBytes + 2) / 3), '\0');
        const int len = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded.data()), random.data(), static_cast<int>(random.size()));
        encoded.resize(static_cast<std::size_t>(len));
        return encoded;
    }

    std::string CryptoUtils::sha256Hex(const std::string &str) {

        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int digestLength = 0;

        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
        EVP_DigestUpdate(ctx, str.data(), str.size());
        EVP_DigestFinal_ex(ctx, digest, &digestLength);
        EVP_MD_CTX_free(ctx);

        return toHex(digest, digestLength);
    }

    std::vector<unsigned char> CryptoUtils::hmacSha256(const std::vector<unsigned char> &key, const std::string &data) {

        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int digestLength = 0;

        HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
             reinterpret_cast<const unsigned char *>(data.data()), data.size(),
             digest, &digestLength);

        return {digest, digest + digestLength};
    }

    std::string CryptoUtils::Base64Encode(const std::string &data) {
        std::string encoded(4 * ((data.size() + 2) / 3), '\0');
        const int len = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded.data()), reinterpret_cast<const unsigned char *>(data.data()), static_cast<int>(data.size()));
        encoded.resize(static_cast<std::size_t>(len));
        return encoded;
    }

    std::string CryptoUtils::Base64Decode(const std::string &data) {
        if (data.empty()) return {};

        std::string decoded(3 * (data.size() / 4), '\0');
        const int len = EVP_DecodeBlock(reinterpret_cast<unsigned char *>(decoded.data()), reinterpret_cast<const unsigned char *>(data.data()), static_cast<int>(data.size()));
        if (len < 0) throw std::runtime_error("Invalid base64 input");

        // EVP_DecodeBlock doesn't strip padding from its output length, so trim it back off here.
        std::size_t padding = 0;
        if (data.size() >= 1 && data[data.size() - 1] == '=') ++padding;
        if (data.size() >= 2 && data[data.size() - 2] == '=') ++padding;
        decoded.resize(static_cast<std::size_t>(len) - padding);
        return decoded;
    }

}// namespace Euclid::Core
