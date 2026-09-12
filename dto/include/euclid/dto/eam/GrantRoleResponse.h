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
#include <euclid/dto/eam/model/Grant.h>

namespace Euclid::Dto::EAM {

    /**
     * @brief The grant that was recorded, with the id revoke-role takes.
     */
    struct GrantRoleResponse : BaseDto {

        /**
         * @brief The grant, as stored.
         */
        Grant grant;

        /**
         * @brief Serializes this to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend GrantRoleResponse tag_invoke(boost::json::value_to_tag<GrantRoleResponse>, boost::json::value const &v) {
            GrantRoleResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.grant = boost::json::value_to<Grant>(v.at("grant"));
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, GrantRoleResponse const &obj) {
            jv = {
                    {"metadata", boost::json::value_from(static_cast<const BaseDto &>(obj))},
                    {"grant", boost::json::value_from(obj.grant)},
            };
        }
    };

}
