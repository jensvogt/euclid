//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EAM {

    struct DeleteUserGroupRequest {

        /**
         * @brief Group name, unique across the deployment
         */
        std::string name;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend DeleteUserGroupRequest tag_invoke(boost::json::value_to_tag<DeleteUserGroupRequest>, boost::json::value const &v) {
            DeleteUserGroupRequest r;
            r.name = Core::GetStringValue(v, "name");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, DeleteUserGroupRequest const &obj) {
            jv = {
                    {"name", obj.name},
            };
        }
    };

}
