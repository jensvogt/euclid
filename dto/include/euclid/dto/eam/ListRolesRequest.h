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
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::EAM {

    using Json::tag_invoke;

    /**
     * @brief Lists the roles the caller's account can bind.
     */
    struct ListRolesRequest {

        /**
         * @brief Only roles whose name starts with this; empty matches all.
         */
        std::string prefix{};

        /**
         * @brief How prefix is matched: "prefix", "endsWith", "contains" or "regex"; empty means
         * "prefix". Every type but "regex" takes prefix literally.
         */
        std::string matchType{};

        /**
         * @brief Maximum to return.
         */
        long pageSize = 10;

        /**
         * @brief Zero-based page index.
         */
        long pageIndex = 0;

        /**
         * @brief Field to sort by.
         */
        std::string sortColumn = "name";

        /**
         * @brief "asc" or "desc".
         */
        std::string sortDirection = "asc";

        /**
         * @brief Whether to include the built-in roles, which are not stored and are not paged. True by default in the handler, since a caller listing roles almost always wants to see what it can bind.
         */
        bool includeBuiltin = true;

        /**
         * @brief The members that keep their initial value when the JSON leaves them out, rather than
         * becoming zero, as the other members do.
         */
        static constexpr std::array<std::string_view, 1> kJsonDefaults{"pageSize"};

        /**
         * @brief Serializes this to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this from a JSON string
         */
        [[nodiscard]] static ListRolesRequest fromJson(const std::string &json) {
            return boost::json::value_to<ListRolesRequest>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(ListRolesRequest, (), (prefix, matchType, pageSize, pageIndex, sortColumn, sortDirection, includeBuiltin))

}
