// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <algorithm>
#include <cctype>

// Euclid includes
#include <euclid/core/CryptoUtils.h>
#include <euclid/core/Federation.h>
#include <euclid/core/JsonUtils.h>
#include <euclid/core/LogStream.h>

namespace Euclid::Core {

    namespace {

        // The AES key a round-trip parameter is sealed with. Derived rather than used directly, so
        // that the JWT signing secret and this never end up being the same bytes even though one
        // configured value stands behind both.
        std::string stateKey(const std::string &secret) {
            return CryptoUtils::sha256Raw("euclid-federation-state:" + secret);
        }

        long nowSeconds() {
            return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

    }// namespace

    std::string SealFederationState(const boost::json::object &payload, const std::string &secret, const std::chrono::seconds ttl) {

        boost::json::object sealed = payload;
        sealed["expiresAt"] = nowSeconds() + ttl.count();

        return CryptoUtils::Base64UrlEncode(CryptoUtils::AesGcmEncrypt(stateKey(secret), boost::json::serialize(sealed)));
    }

    std::optional<boost::json::object> OpenFederationState(const std::string &sealed, const std::string &secret) {

        try {
            const auto plaintext = CryptoUtils::AesGcmDecrypt(stateKey(secret), CryptoUtils::Base64UrlDecode(sealed));
            auto parsed = boost::json::parse(plaintext);
            if (!parsed.is_object()) return std::nullopt;

            if (const auto expiresAt = GetLongValue(parsed, "expiresAt"); expiresAt <= nowSeconds()) {
                log_warning << "Federation state expired, expiresAt: " << expiresAt << ", now: " << nowSeconds();
                return std::nullopt;
            }
            return parsed.as_object();

        } catch (const std::exception &e) {
            // Failing to decrypt is the same answer as being expired, and for the caller the same
            // answer as never having been ours: the login does not proceed. Logged at warning
            // because in an installation that is working it should not happen at all.
            log_warning << "Federation state could not be opened, error: " << e.what();
            return std::nullopt;
        }
    }

    bool IsLoopbackUrl(const std::string &url) {

        // Plain HTTP only, and only these three names. A loopback address is where the CLI listens
        // for its own login (RFC 8252 says a native client should), and a redirect there cannot
        // leave the machine the person is sitting at - which is exactly why it needs no
        // configuring, and why it must be recognised precisely rather than by "contains
        // localhost", which "https://localhost.evil.test/" also does.
        for (const auto *prefix: {"http://127.0.0.1", "http://[::1]", "http://localhost"}) {
            const std::string candidate(prefix);
            if (!url.starts_with(candidate)) continue;

            // What follows the host has to start a port or a path, so that "http://localhost.evil"
            // is not read as loopback.
            const auto rest = url.substr(candidate.size());
            if (rest.empty() || rest.starts_with(":") || rest.starts_with("/") || rest.starts_with("?")) return true;
        }
        return false;
    }

    bool IsReturnToAllowed(const std::string &returnTo, const std::vector<std::string> &prefixes) {

        if (returnTo.empty()) return true;

        // A path on the gateway itself needs no configuring, because it is where the request
        // already is. "//host/path" is excluded deliberately: a browser reads it as another
        // origin, so it is an absolute URL wearing a path's clothes.
        if (returnTo.starts_with("/") && !returnTo.starts_with("//")) return true;

        if (IsLoopbackUrl(returnTo)) return true;

        return std::ranges::any_of(prefixes, [&returnTo](const std::string &prefix) {
            // Prefix rather than origin comparison, so an installation can narrow this to one page
            // rather than a whole host. The prefix has to end at a path boundary, or one written
            // for "https://console.example.com" would also admit
            // "https://console.example.com.evil.test".
            if (prefix.empty()) return false;
            const auto boundedPrefix = prefix.ends_with("/") ? prefix : prefix + "/";
            return returnTo == prefix || returnTo.starts_with(boundedPrefix);
        });
    }

    std::string SanitizeUserId(const std::string &claim) {

        std::string sanitized;
        sanitized.reserve(claim.size());
        for (const char c: claim) {
            const bool acceptable = std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_' || c == '@' || c == '+';
            sanitized += acceptable ? c : '-';
        }
        return sanitized;
    }

}// namespace Euclid::Core
