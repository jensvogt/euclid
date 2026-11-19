// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/12/26.
//

#pragma once

// C++ includes
#include <string>
#include <vector>

// Euclid includes
#include <euclid/core/DateTimeUtils.h>
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::EAM {

    using Json::tag_invoke;

    /**
     * @brief A named set of permissions, belonging to one account.
     *
     * @par
     * "builtin" tells a caller which roles it may change: the built-in roles are computed rather
     * than stored, and create/update/delete refuse them. Without it a UI has to know the six names.
     */
    struct Role {

        std::string name;
        std::string ern;
        std::string accountId;
        std::string region;
        std::string description;
        std::vector<std::string> permissions;
        bool builtin{false};
        std::string created;
        std::string modified;

        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(Role, (), (name, ern, accountId, region, description, permissions, builtin, created, modified))

}
