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
     * @brief Removes one grant.
     */
    struct RevokeRoleRequest {

        /**
         * @brief The grant's own id, as grant-role and list-grants report it. Not the (role, principal) pair: the same role may be granted to the same principal twice with different scope, and revoking has to say which.
         */
        std::string grantId;

        /**
         * @brief Serializes this to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this from a JSON string
         */
        [[nodiscard]] static RevokeRoleRequest fromJson(const std::string &json) {
            return boost::json::value_to<RevokeRoleRequest>(Core::ParseJsonString(json));
        }

    private:

        friend RevokeRoleRequest tag_invoke(boost::json::value_to_tag<RevokeRoleRequest>, boost::json::value const &v) {
            RevokeRoleRequest r;
            r.grantId = Core::GetStringValue(v, "grantId");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, RevokeRoleRequest const &obj) {
            jv = {
                    {"grantId", obj.grantId},
            };
        }
    };

}
