// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/19/26.
//

#pragma once

// C++ includes
#include <array>
#include <string>
#include <string_view>

// Euclid includes
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/eam/model/UserGroup.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::EAM {

    using Json::tag_invoke;

    /**
     * @brief One user group, as list-user-groups describes each of its own.
     */
    struct GetUserGroupResponse : BaseDto {

        /**
         * @brief The group.
         */
        UserGroup userGroup;

        /**
         * @brief The members that must be in the JSON: missing or null, reading throws.
         */
        static constexpr std::array<std::string_view, 1> kJsonRequired{"userGroup"};

        /**
         * @brief Serializes this response to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(GetUserGroupResponse, (BaseDto), (userGroup))
}// namespace Euclid::Dto::EAM
