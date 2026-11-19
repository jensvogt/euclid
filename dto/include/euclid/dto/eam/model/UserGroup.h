// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <string>
#include <vector>

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto {

    using Json::tag_invoke;

    struct UserGroup {

        /**
         * @brief Group name
         */
        std::string name;

        /**
         * @brief Euclid resource name
         */
        std::string ern;

        /**
         * @brief Account the group belongs to
         */
        std::string accountId;

        /**
         * @brief Region the group was created in
         */
        std::string region;

        /**
         * @brief Free-text description of the group's purpose
         */
        std::string description;

        /**
         * @brief User IDs currently in this group
         */
        std::vector<std::string> userIds;

        /**
         * @brief Creation timestamp
         */
        std::chrono::system_clock::time_point created;

        /**
         * @brief Modification timestamp
         */
        std::chrono::system_clock::time_point modified;
    };

    BOOST_DESCRIBE_STRUCT(UserGroup, (), (name, ern, accountId, region, description, userIds, created, modified))
}