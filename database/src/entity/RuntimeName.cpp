// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/11/26.
//

// Euclid includes
#include <euclid/core/CryptoUtils.h>
#include <euclid/database/entity/RuntimeName.h>

namespace Euclid::Database::Entity {

    std::string GenerateRuntimeName(const std::string &id) {
        constexpr std::size_t kMaxPrefix = 32;
        const auto prefix = id.size() > kMaxPrefix ? id.substr(0, kMaxPrefix) : id;
        return prefix + "-" + Core::CryptoUtils::GenerateShortId();
    }

}// namespace Euclid::Database::Entity
