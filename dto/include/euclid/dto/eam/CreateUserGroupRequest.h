//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EAM {

    struct CreateUserGroupRequest {

        /**
         * @brief Group name, unique across the deployment
         */
        std::string name;

        /**
         * @brief Free-text description of the group's purpose
         */
        std::string description;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend CreateUserGroupRequest tag_invoke(boost::json::value_to_tag<CreateUserGroupRequest>, boost::json::value const &v) {
            CreateUserGroupRequest r;
            r.name = Core::GetStringValue(v, "name");
            r.description = Core::GetStringValue(v, "description");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, CreateUserGroupRequest const &obj) {
            jv = {
                    {"name", obj.name},
                    {"description", obj.description},
            };
        }
    };

}
