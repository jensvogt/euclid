//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <string>

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto {

    struct User {
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
         * @brief Account the user belongs to
         */
        std::string accountId;

        /**
         * @brief Region the user was registered in
         */
        std::string region;

        /**
         * @brief Administrator flag
         */
        bool isAdmin{false};

    private:

        friend User tag_invoke(boost::json::value_to_tag<User>, boost::json::value const &v) {
            User r;
            r.userId = Core::GetStringValue(v, "userId");
            r.password = Core::GetStringValue(v, "password");
            r.email = Core::GetStringValue(v, "email");
            r.accountId = Core::GetStringValue(v, "accountId");
            r.region = Core::GetStringValue(v, "region");
            r.isAdmin = Core::GetBoolValue(v, "isAdmin");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, User const &obj) {
            jv = {
                    {"userId", obj.userId},
                    {"password", obj.password},
                    {"email", obj.email},
                    {"accountId", obj.accountId},
                    {"region", obj.region},
                    {"isAdmin", obj.isAdmin},
            };
        }
    };
}