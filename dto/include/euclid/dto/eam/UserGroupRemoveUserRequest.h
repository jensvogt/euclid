//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EAM {

    struct UserGroupRemoveUserRequest {

        /**
         * @brief User group ERN
         */
        std::string userGroup;

        /**
         * @brief User ERN
         */
        std::string user;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend UserGroupRemoveUserRequest tag_invoke(boost::json::value_to_tag<UserGroupRemoveUserRequest>, boost::json::value const &v) {
            UserGroupRemoveUserRequest r;
            r.userGroup = Core::GetStringValue(v, "userGroup");
            r.user = Core::GetStringValue(v, "user");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, UserGroupRemoveUserRequest const &obj) {
            jv = {
                    {"userGroup", obj.userGroup},
                    {"user", obj.user},
            };
        }
    };

}