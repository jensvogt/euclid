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
     * @brief Deletes a role.
     *
     * @par
     * Refused while any grant still names it, rather than leaving grants pointing at nothing -
     * revoke them first, which is a decision somebody should make deliberately.
     */
    struct DeleteRoleRequest {

        /**
         * @brief Role name.
         */
        std::string name;

        /**
         * @brief Serializes this to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this from a JSON string
         */
        [[nodiscard]] static DeleteRoleRequest fromJson(const std::string &json) {
            return boost::json::value_to<DeleteRoleRequest>(Core::ParseJsonString(json));
        }

    private:

        friend DeleteRoleRequest tag_invoke(boost::json::value_to_tag<DeleteRoleRequest>, boost::json::value const &v) {
            DeleteRoleRequest r;
            r.name = Core::GetStringValue(v, "name");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, DeleteRoleRequest const &obj) {
            jv = {
                    {"name", obj.name},
            };
        }
    };

}
