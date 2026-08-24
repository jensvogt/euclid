//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <string>

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EAM {

    struct DeleteUserRequest {

        /**
         * @brief User ID prefix
         */
        std::string userId{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend DeleteUserRequest tag_invoke(boost::json::value_to_tag<DeleteUserRequest>, boost::json::value const &v) {
            DeleteUserRequest r;
            r.userId = Core::GetStringValue(v, "userId");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, DeleteUserRequest const &obj) {
            jv = {
                    {"userId", obj.userId},
            };
        }
    };

}
