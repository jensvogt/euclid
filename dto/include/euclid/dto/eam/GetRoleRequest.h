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
     * @brief Reads one role, stored or built-in.
     */
    struct GetRoleRequest {

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
        [[nodiscard]] static GetRoleRequest fromJson(const std::string &json) {
            return boost::json::value_to<GetRoleRequest>(Core::ParseJsonString(json));
        }

    private:

        friend GetRoleRequest tag_invoke(boost::json::value_to_tag<GetRoleRequest>, boost::json::value const &v) {
            GetRoleRequest r;
            r.name = Core::GetStringValue(v, "name");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, GetRoleRequest const &obj) {
            jv = {
                    {"name", obj.name},
            };
        }
    };

}
