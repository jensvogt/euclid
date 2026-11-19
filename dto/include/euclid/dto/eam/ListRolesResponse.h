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
     * @brief One page of roles, and how many exist.
     *
     * @par
     * "total" counts the account's stored roles. The built-in ones are not stored and not paged -
     * they are prepended when asked for, so a page of ten stored roles arrives with them on top.
     */
    struct ListRolesResponse : BaseDto {

        /**
         * @brief The roles, built-in ones first when they were asked for.
         */
        std::vector<Role> roles;

        /**
         * @brief How many stored roles the account has.
         */
        long total{};

        /**
         * @brief The members that must be in the JSON: missing or null, reading throws.
         */
        static constexpr std::array<std::string_view, 1> kJsonRequired{"roles"};

        /**
         * @brief Serializes this to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(ListRolesResponse, (BaseDto), (roles, total))

}
