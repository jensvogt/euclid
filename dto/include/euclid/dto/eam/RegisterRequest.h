//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <string>

// Boost includes
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EAM {

    struct RegisterRequest {

        /**
         * @brief User ID
         */
        std::string userId;

        /**
         * @brief User password (plaintext, hashed before storage)
         */
        std::string password;

        /**
         * @brief User email
         */
        std::string email;

        /**
         * @brief Account the new user is registered into
         */
        std::string accountId;

        /**
         * @brief Region the new user is registered in
         */
        std::string region;

        /**
         * @brief Whether the new user should have administrator privileges.
         *
         * Ignored unless the caller is itself an administrator - the exception is the
         * very first user ever registered, which always becomes an administrator.
         */
        bool isAdmin{false};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend RegisterRequest tag_invoke(boost::json::value_to_tag<RegisterRequest>, boost::json::value const &v) {
            RegisterRequest r;
            r.userId = Core::GetStringValue(v, "userId");
            r.password = Core::GetStringValue(v, "password");
            r.email = Core::GetStringValue(v, "email");
            r.accountId = Core::GetStringValue(v, "accountId");
            r.region = Core::GetStringValue(v, "region");
            r.isAdmin = Core::GetBoolValue(v, "isAdmin");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, RegisterRequest const &obj) {
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
