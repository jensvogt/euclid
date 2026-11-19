// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/12/26.
//

#pragma once

// C++ includes
#include <array>
#include <string>
#include <string_view>
#include <vector>

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/eam/model/Role.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::EAM {

    using Json::tag_invoke;

    /**
     * @brief One role - what create-role, get-role and update-role answer with.
     */
    struct RoleResponse : BaseDto {

        /**
         * @brief The role, with its permissions as stored.
         */
        Role role;

        /**
         * @brief The members that must be in the JSON: missing or null, reading throws.
         */
        static constexpr std::array<std::string_view, 1> kJsonRequired{"role"};

        /**
         * @brief Serializes this to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(RoleResponse, (BaseDto), (role))

}
