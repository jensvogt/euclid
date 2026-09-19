// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/19/26.
//

#pragma once

// C++ includes
#include <string>

// Euclid includes
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/eam/model/UserGroup.h>

namespace Euclid::Dto::EAM {

    /**
     * @brief One user group, as list-user-groups describes each of its own.
     */
    struct GetUserGroupResponse : BaseDto {

        /**
         * @brief The group.
         */
        UserGroup userGroup;

        /**
         * @brief Serializes this response to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend GetUserGroupResponse tag_invoke(boost::json::value_to_tag<GetUserGroupResponse>, boost::json::value const &v) {
            GetUserGroupResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.userGroup = boost::json::value_to<UserGroup>(v.at("userGroup"));
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, GetUserGroupResponse const &obj) {
            jv = {
                    {"userGroup", boost::json::value_from(obj.userGroup)},
            };
        }
    };
}// namespace Euclid::Dto::EAM
