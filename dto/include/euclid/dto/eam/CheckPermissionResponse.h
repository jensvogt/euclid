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

namespace Euclid::Dto::EAM {

    /**
     * @brief Whether a user would be allowed, and why.
     */
    struct CheckPermissionResponse : BaseDto {

        /**
         * @brief Whether the request would be allowed.
         */
        bool allowed{false};

        /**
         * @brief In one line, what decided it - the role that allowed it, or what was missing.
         */
        std::string reason;

        /**
         * @brief The role whose grant allowed it, when one did. Empty on a refusal.
         */
        std::string role;

        /**
         * @brief Serializes this to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend CheckPermissionResponse tag_invoke(boost::json::value_to_tag<CheckPermissionResponse>, boost::json::value const &v) {
            CheckPermissionResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.allowed = Core::GetBoolValue(v, "allowed");
            r.reason = Core::GetStringValue(v, "reason");
            r.role = Core::GetStringValue(v, "role");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, CheckPermissionResponse const &obj) {
            jv = {
                    {"metadata", boost::json::value_from(static_cast<const BaseDto &>(obj))},
                    {"allowed", obj.allowed},
                    {"reason", obj.reason},
                    {"role", obj.role},
            };
        }
    };

}
