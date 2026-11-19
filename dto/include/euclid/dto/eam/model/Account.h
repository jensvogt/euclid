// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <string>

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto {

    using Json::tag_invoke;

    struct Account {

        /**
         * @brief Account ID, unique across the deployment
         */
        std::string accountId;

        /**
         * @brief Human-readable account name
         */
        std::string name;

        /**
         * @brief Euclid resource name
         */
        std::string ern;

        /**
         * @brief Free-text description of the account's purpose
         */
        std::string description;

        /**
         * @brief Creation timestamp
         */
        std::chrono::system_clock::time_point created;

        /**
         * @brief Modification timestamp
         */
        std::chrono::system_clock::time_point modified;
    };

    BOOST_DESCRIBE_STRUCT(Account, (), (accountId, name, ern, description, created, modified))
}
