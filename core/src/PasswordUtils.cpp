// C++ includes
#include <array>
#include <iomanip>
#include <sstream>
#include <vector>

// OpenSSL includes
#include <openssl/evp.h>
#include <openssl/rand.h>

// Euclid includes
#include <euclid/core/PasswordUtils.h>

namespace Euclid::Core {

    namespace {

        constexpr int kIterations = 100000;
        constexpr int kSaltBytes = 16;
        constexpr int kHashBytes = 32;

        std::string toHex(const unsigned char *data, const std::size_t len) {
            std::ostringstream oss;
            oss << std::hex << std::setfill('0');
            for (std::size_t i = 0; i < len; ++i) oss << std::setw(2) << static_cast<int>(data[i]);
            return oss.str();
        }

        std::vector<unsigned char> fromHex(const std::string &hex) {
            std::vector<unsigned char> bytes(hex.size() / 2);
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                bytes[i] = static_cast<unsigned char>(std::stoi(hex.substr(i * 2, 2), nullptr, 16));
            }
            return bytes;
        }

        std::vector<std::string> split(const std::string &s, const char delim) {
            std::vector<std::string> parts;
            std::stringstream ss(s);
            std::string part;
            while (std::getline(ss, part, delim)) parts.push_back(part);
            return parts;
        }

        // Constant-time comparison to avoid leaking hash contents via timing.
        bool constantTimeEquals(const std::vector<unsigned char> &a, const std::vector<unsigned char> &b) {
            if (a.size() != b.size()) return false;
            unsigned char diff = 0;
            for (std::size_t i = 0; i < a.size(); ++i) diff |= a[i] ^ b[i];
            return diff == 0;
        }

        std::vector<unsigned char> pbkdf2(const std::string &password, const std::vector<unsigned char> &salt, const int iterations) {
            std::vector<unsigned char> hash(kHashBytes);
            PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                               salt.data(), static_cast<int>(salt.size()),
                               iterations, EVP_sha256(),
                               kHashBytes, hash.data());
            return hash;
        }

    }// namespace

    std::string PasswordUtils::Hash(const std::string &password) {

        std::array<unsigned char, kSaltBytes> salt{};
        RAND_bytes(salt.data(), kSaltBytes);

        const std::vector<unsigned char> saltVec(salt.begin(), salt.end());
        const auto hash = pbkdf2(password, saltVec, kIterations);

        return "pbkdf2-sha256$" + std::to_string(kIterations) + "$" + toHex(salt.data(), salt.size()) + "$" + toHex(hash.data(), hash.size());
    }

    bool PasswordUtils::Verify(const std::string &password, const std::string &hash) {

        const auto parts = split(hash, '$');
        if (parts.size() != 4 || parts[0] != "pbkdf2-sha256") return false;

        try {
            const int iterations = std::stoi(parts[1]);
            const auto salt = fromHex(parts[2]);
            const auto expected = fromHex(parts[3]);
            const auto actual = pbkdf2(password, salt, iterations);
            return constantTimeEquals(actual, expected);
        } catch (const std::exception &) {
            return false;
        }
    }

}// namespace Euclid::Core
