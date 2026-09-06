//
// Created by vogje01 on 9/6/26.
//

// C++ includes
#include <optional>

// Euclid includes
#include <BasicAuthenticator.h>
#include <euclid/core/CryptoUtils.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/PasswordUtils.h>
#include <euclid/database/RepositoryFactory.h>

namespace Euclid::EAG {

    BasicAuthenticator::BasicAuthenticator(const long cacheSeconds) : _cacheFor(std::max(0L, cacheSeconds)) {}

    std::optional<std::string> BasicAuthenticator::Verify(const std::string &authorization) {

        constexpr std::string_view basicPrefix = "Basic ";
        if (!authorization.starts_with(basicPrefix)) return std::nullopt;

        const auto encoded = authorization.substr(basicPrefix.size());
        if (encoded.empty()) return std::nullopt;

        // Keyed by a hash of the header, so what sits in this process's memory for the next minute
        // is not a list of passwords.
        const auto cacheKey = Core::CryptoUtils::sha256Hex(encoded);

        if (_cacheFor.count() > 0) {
            std::lock_guard lock(_mutex);
            if (const auto it = _verified.find(cacheKey); it != _verified.end()) {
                if (std::chrono::steady_clock::now() < it->second.expiresAt) return it->second.userId;
                _verified.erase(it);
            }
        }

        std::string decoded;
        try {
            decoded = Core::CryptoUtils::Base64Decode(encoded);
        } catch (const std::exception &) {
            log_debug << "Basic authentication: credentials are not valid base64";
            return std::nullopt;
        }

        // Split on the first colon only: a password may contain one, a username may not.
        const auto colon = decoded.find(':');
        if (colon == std::string::npos) {
            log_debug << "Basic authentication: credentials are not user:password";
            return std::nullopt;
        }
        const auto userId = decoded.substr(0, colon);
        const auto password = decoded.substr(colon + 1);
        if (userId.empty() || password.empty()) return std::nullopt;

        const auto user = Database::RepositoryFactory::instance().eamRepository()->findUserByUserId(userId);
        if (!user.has_value()) {
            // Logged, not answered: which of these failures it was is exactly what somebody
            // probing for valid usernames wants to learn, so the caller sees one 401 for all of
            // them.
            log_warning << "Basic authentication failed, no such user: " << userId;
            return std::nullopt;
        }

        // Checked before the password rather than after, unlike eam login, which deliberately
        // checks it afterwards so the two kinds of account cannot be told apart by timing. Here
        // there is nothing to hide: a technical principal is an application's identity, it has an
        // access key to sign with, and it has no password anybody could be probing for.
        if (!user->loginEnabled) {
            log_warning << "Basic authentication refused for technical user: " << userId;
            return std::nullopt;
        }

        if (!Core::PasswordUtils::Verify(password, user->password)) {
            log_warning << "Basic authentication failed, bad password for user: " << userId;
            return std::nullopt;
        }

        if (_cacheFor.count() > 0) {
            std::lock_guard lock(_mutex);

            // Swept here rather than on a timer: the map only grows when somebody authenticates,
            // so the moment somebody does is the right moment to drop what has expired. Without
            // this a gateway seeing many distinct credentials would keep every one of them
            // forever.
            const auto now = std::chrono::steady_clock::now();
            std::erase_if(_verified, [&now](const auto &entry) { return now >= entry.second.expiresAt; });

            _verified[cacheKey] = {.userId = user->userId, .expiresAt = now + _cacheFor};
        }

        log_debug << "Basic authentication succeeded, userId: " << user->userId;
        return user->userId;
    }

}// namespace Euclid::EAG
