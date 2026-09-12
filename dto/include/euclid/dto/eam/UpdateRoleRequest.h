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

namespace Euclid::Dto::EAM {

    /**
     * @brief Replaces a role's permissions and description.
     *
     * @par
     * Replaces rather than merges: what the role grants afterwards is exactly what is sent, so a
     * permission left out is taken away. A merge would make removing one impossible.
     */
    struct UpdateRoleRequest {

        /**
         * @brief Role name.
         */
        std::string name;

        /**
         * @brief Free-text description.
         */
        std::string description;

        /**
         * @brief The role's permissions after this call.
         */
        std::vector<std::string> permissions;

        /**
         * @brief Serializes this to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this from a JSON string
         */
        [[nodiscard]] static UpdateRoleRequest fromJson(const std::string &json) {
            return boost::json::value_to<UpdateRoleRequest>(Core::ParseJsonString(json));
        }

    private:

        friend UpdateRoleRequest tag_invoke(boost::json::value_to_tag<UpdateRoleRequest>, boost::json::value const &v) {
            UpdateRoleRequest r;
            r.name = Core::GetStringValue(v, "name");
            r.description = Core::GetStringValue(v, "description");
            r.permissions = Core::GetStringArrayValue(v, "permissions");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, UpdateRoleRequest const &obj) {
            jv = {
                    {"name", obj.name},
                    {"description", obj.description},
                    {"permissions", boost::json::value_from(obj.permissions)},
            };
        }
    };

}
