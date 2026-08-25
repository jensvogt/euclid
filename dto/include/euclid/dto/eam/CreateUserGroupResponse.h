//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid include
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/eam/model/UserGroup.h>

namespace Euclid::Dto::EAM {

    struct CreateUserGroupResponse : BaseDto {

        /**
         * @brief The newly created group
         */
        UserGroup userGroup;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend CreateUserGroupResponse tag_invoke(boost::json::value_to_tag<CreateUserGroupResponse>, boost::json::value const &v) {
            CreateUserGroupResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.userGroup = boost::json::value_to<UserGroup>(v.at("userGroup"));
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, CreateUserGroupResponse const &obj) {
            jv = {
                    {"userGroup", boost::json::value_from(obj.userGroup)},
            };
        }
    };

}