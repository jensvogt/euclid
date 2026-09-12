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
     * @brief Gives a role to a principal, scoped.
     */
    struct GrantRoleRequest {

        /**
         * @brief Role name - one of the account's own, or a built-in.
         */
        std::string role;

        /**
         * @brief Who gets it: a user ERN or a user-group ERN. One field for both, because the ERN says which.
         */
        std::string principal;

        /**
         * @brief Namespaces of the account it applies in; a single "*" means all of them. Empty is refused - a grant that grants nowhere is a mistake, not a configuration.
         */
        std::vector<std::string> namespaces;

        /**
         * @brief ERN patterns it applies to, each exact or ending in "*"; a single "*" means all. Empty means the grant covers only the actions that name no resource.
         */
        std::vector<std::string> resources;

        /**
         * @brief Serializes this to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this from a JSON string
         */
        [[nodiscard]] static GrantRoleRequest fromJson(const std::string &json) {
            return boost::json::value_to<GrantRoleRequest>(Core::ParseJsonString(json));
        }

    private:

        friend GrantRoleRequest tag_invoke(boost::json::value_to_tag<GrantRoleRequest>, boost::json::value const &v) {
            GrantRoleRequest r;
            r.role = Core::GetStringValue(v, "role");
            r.principal = Core::GetStringValue(v, "principal");
            r.namespaces = Core::GetStringArrayValue(v, "namespaces");
            r.resources = Core::GetStringArrayValue(v, "resources");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, GrantRoleRequest const &obj) {
            jv = {
                    {"role", obj.role},
                    {"principal", obj.principal},
                    {"namespaces", boost::json::value_from(obj.namespaces)},
                    {"resources", boost::json::value_from(obj.resources)},
            };
        }
    };

}
