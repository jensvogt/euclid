//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <string>
#include <vector>

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/eam/model/AccountGrant.h>

namespace Euclid::Dto {

    struct User {
        /**
         * @brief User ID
         */
        std::string userId;

        /**
         * @brief Euclid resource name
         */
        std::string ern;

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
         * @brief Explicit per-(account, namespace) grants held by this user, in addition to
         * accountId above (the user's home account)
         */
        std::vector<AccountGrant> accountGrants;

        /**
         * @brief Created timestamp
         */
        std::chrono::system_clock::time_point created;

        /**
         * @brief Created timestamp
         */
        std::chrono::system_clock::time_point modified;

    private:

        friend User tag_invoke(boost::json::value_to_tag<User>, boost::json::value const &v) {
            User r;
            r.userId = Core::GetStringValue(v, "userId");
            r.ern = Core::GetStringValue(v, "ern");
            r.password = Core::GetStringValue(v, "password");
            r.email = Core::GetStringValue(v, "email");
            r.accountId = Core::GetStringValue(v, "accountId");
            r.region = Core::GetStringValue(v, "region");
            if (Core::AttributeExists(v, "accountGrants")) {
                r.accountGrants = boost::json::value_to<std::vector<AccountGrant> >(v.at("accountGrants"));
            }
            r.created = Core::GetDatetimeValue(v, "created");
            r.modified = Core::GetDatetimeValue(v, "modified");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, User const &obj) {
            jv = {
                    {"userId", obj.userId},
                    {"ern", obj.ern},
                    {"password", obj.password},
                    {"email", obj.email},
                    {"accountId", obj.accountId},
                    {"region", obj.region},
                    {"accountGrants", boost::json::value_from(obj.accountGrants)},
                    {"created", Core::DateTimeUtils::ToISO8601(obj.created)},
                    {"modified", Core::DateTimeUtils::ToISO8601(obj.modified)},
            };
        }
    };
}