//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <string>

// Boost includes
#include <boost/json/value.hpp>

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::Access {

    struct LoginRequest {

        /**
         * @brief User ID
         */
        std::string userId;

        /**
         * @brief User password
         */
        std::string password;

        /**
         * @brief User email
         */
        std::string email;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend LoginRequest tag_invoke(boost::json::value_to_tag<LoginRequest>, boost::json::value const &v) {
            LoginRequest r;
            r.userId = Core::GetStringValue(v, "userId");
            r.password = Core::GetStringValue(v, "password");
            r.email = Core::GetStringValue(v, "email");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, LoginRequest const &obj) {
            jv = {
                    {"userId", obj.userId},
                    {"password", obj.password},
                    {"email", obj.email},
            };
        }
    };

}
