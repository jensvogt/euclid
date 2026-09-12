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
     * @brief One role - what create-role, get-role and update-role answer with.
     */
    struct RoleResponse : BaseDto {

        /**
         * @brief The role, with its permissions as stored.
         */
        Role role;

        /**
         * @brief Serializes this to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend RoleResponse tag_invoke(boost::json::value_to_tag<RoleResponse>, boost::json::value const &v) {
            RoleResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.role = boost::json::value_to<Role>(v.at("role"));
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, RoleResponse const &obj) {
            jv = {
                    {"metadata", boost::json::value_from(static_cast<const BaseDto &>(obj))},
                    {"role", boost::json::value_from(obj.role)},
            };
        }
    };

}
