//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <vector>

// Euclid includes
#include "model/UserGroup.h"


#include <euclid/dto/BaseDto.h>
#include <euclid/dto/eam/model/User.h>

namespace Euclid::Dto::EAM {

    struct ListUserGroupsResponse : BaseDto {

        /**
         * @brief User groups
         */
        std::vector<UserGroup> userGroups;

        /**
         * @brief total number of users
         */
        long total{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend ListUserGroupsResponse tag_invoke(boost::json::value_to_tag<ListUserGroupsResponse>, boost::json::value const &v) {
            ListUserGroupsResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.userGroups = boost::json::value_to<std::vector<UserGroup> >(v.at("userGroups"));
            r.total = Core::GetLongValue(v, "total");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ListUserGroupsResponse const &obj) {
            jv = {
                    {"userGroups", boost::json::value_from(obj.userGroups)},
                    {"total", obj.total},
            };
        }
    };

}