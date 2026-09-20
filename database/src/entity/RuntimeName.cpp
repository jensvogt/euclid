// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/11/26.
//

// C++ includes
#include <stdexcept>

// Euclid includes
#include <euclid/core/CryptoUtils.h>
#include <euclid/database/entity/RuntimeName.h>

namespace Euclid::Database::Entity {

    namespace {
        // The cap is on the whole name rather than on the generated part: all of this ends up in a
        // unix socket path, and sun_path is 108 bytes.
        constexpr std::size_t kMaxPrefix = 32;

        std::string truncated(const std::string &id) {
            return id.size() > kMaxPrefix ? id.substr(0, kMaxPrefix) : id;
        }
    }// namespace

    std::string GenerateRuntimeName(const std::string &id) {
        return truncated(id) + "-" + Core::CryptoUtils::GenerateShortId();
    }

    bool IsSafeRuntimeName(const std::string &name) {
        if (name.empty() || name == "." || name == "..") return false;
        return name.find('/') == std::string::npos &&
               name.find('\\') == std::string::npos &&
               name.find('\0') == std::string::npos;
    }

    std::string IssueRuntimeName(const std::string &id, const std::function<bool(const std::string &)> &taken) {

        if (const auto plain = truncated(id); IsSafeRuntimeName(plain) && !taken(plain)) return plain;

        // Taken, or an id that would not make a usable path on its own. The suffix is random, so
        // "unlikely" is not "cannot" - hence the retries rather than one attempt.
        for (int attempt = 0; attempt < 8; ++attempt) {
            auto candidate = GenerateRuntimeName(id);
            if (IsSafeRuntimeName(candidate) && !taken(candidate)) return candidate;
        }
        throw std::runtime_error("could not find a free runtime name for " + id);
    }

}// namespace Euclid::Database::Entity
