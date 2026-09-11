// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <stdexcept>

// OpenSSL includes
#include <openssl/hmac.h>

// Euclid includes
#include <euclid/core/Totp.h>

namespace Euclid::Core {

    std::string Totp::Base32Decode(const std::string &encoded) {

        static constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

        std::string decoded;
        std::uint32_t buffer = 0;
        int bits = 0;

        for (const char raw: encoded) {

            // Providers print secrets in groups, and people paste them with the padding still on.
            // Neither carries information, so neither is an error.
            if (raw == '=' || std::isspace(static_cast<unsigned char>(raw)) || raw == '-') continue;

            const auto position = alphabet.find(static_cast<char>(std::toupper(static_cast<unsigned char>(raw))));
            if (position == std::string_view::npos) throw std::runtime_error("Not a base32 secret");

            buffer = (buffer << 5) | static_cast<std::uint32_t>(position);
            bits += 5;
            if (bits >= 8) {
                bits -= 8;
                decoded += static_cast<char>((buffer >> bits) & 0xFF);
            }
        }
        return decoded;
    }

    std::string Totp::Code(const std::string &secretBase32, const std::chrono::system_clock::time_point when,
                           const int digits, const std::chrono::seconds step) {

        return CodeAt(secretBase32, std::chrono::duration_cast<std::chrono::seconds>(when.time_since_epoch()), digits, step);
    }

    std::string Totp::CodeAt(const std::string &secretBase32, const std::chrono::seconds unixTime,
                             const int digits, const std::chrono::seconds step) {

        if (digits < 1 || digits > 9) throw std::runtime_error("A one-time password has between one and nine digits");
        if (step.count() <= 0) throw std::runtime_error("A one-time password step has to be positive");

        const auto secret = Base32Decode(secretBase32);
        if (secret.empty()) throw std::runtime_error("The one-time password secret is empty");

        // The counter is the number of whole steps since the epoch, big-endian in eight bytes -
        // which is what makes this HOTP with the clock as its counter.
        auto counter = static_cast<std::uint64_t>(unixTime.count() / step.count());

        std::array<unsigned char, 8> message{};
        for (int i = 7; i >= 0; --i) {
            message[static_cast<std::size_t>(i)] = static_cast<unsigned char>(counter & 0xFF);
            counter >>= 8;
        }

        // SHA-1, because that is what RFC 6238 specifies and what every authenticator enrolled
        // against a provider actually uses. Its weaknesses are collision weaknesses; this is an
        // HMAC over eight bytes with a shared secret, where they do not apply.
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int digestLength = 0;
        HMAC(EVP_sha1(), secret.data(), static_cast<int>(secret.size()), message.data(), message.size(), digest, &digestLength);

        // Dynamic truncation: the low nibble of the last byte picks where to read four bytes from.
        const std::size_t offset = digest[digestLength - 1] & 0x0F;
        const std::uint32_t binary = (static_cast<std::uint32_t>(digest[offset] & 0x7F) << 24) |
                                     (static_cast<std::uint32_t>(digest[offset + 1]) << 16) |
                                     (static_cast<std::uint32_t>(digest[offset + 2]) << 8) |
                                     static_cast<std::uint32_t>(digest[offset + 3]);

        std::uint32_t modulus = 1;
        for (int i = 0; i < digits; ++i) modulus *= 10;

        auto code = std::to_string(binary % modulus);
        code.insert(code.begin(), static_cast<std::size_t>(digits) - code.size(), '0');
        return code;
    }

}// namespace Euclid::Core
