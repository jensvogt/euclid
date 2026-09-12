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
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/eam/model/Role.h>

namespace Euclid::Dto::EAM {

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
         * @brief Serializes this to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend ListRolesResponse tag_invoke(boost::json::value_to_tag<ListRolesResponse>, boost::json::value const &v) {
            ListRolesResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            for (const auto &role: v.at("roles").as_array()) r.roles.push_back(boost::json::value_to<Role>(role));
            r.total = Core::GetLongValue(v, "total");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ListRolesResponse const &obj) {
            jv = {
                    {"metadata", boost::json::value_from(static_cast<const BaseDto &>(obj))},
                    {"roles", boost::json::value_from(obj.roles)},
                    {"total", obj.total},
            };
        }
    };

}
