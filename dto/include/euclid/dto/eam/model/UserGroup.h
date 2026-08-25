//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <string>
#include <vector>

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto {

    struct UserGroup {

        /**
         * @brief Group name
         */
        std::string name;

        /**
         * @brief Euclid resource name
         */
        std::string ern;

        /**
         * @brief Account the group belongs to
         */
        std::string accountId;

        /**
         * @brief Region the group was created in
         */
        std::string region;

        /**
         * @brief Free-text description of the group's purpose
         */
        std::string description;

        /**
         * @brief User IDs currently in this group
         */
        std::vector<std::string> userIds;

        /**
         * @brief Creation timestamp, ISO8601
         */
        std::string createdAt;

    private:

        friend UserGroup tag_invoke(boost::json::value_to_tag<UserGroup>, boost::json::value const &v) {
            UserGroup r;
            r.name = Core::GetStringValue(v, "name");
            r.ern = Core::GetStringValue(v, "ern");
            r.accountId = Core::GetStringValue(v, "accountId");
            r.region = Core::GetStringValue(v, "region");
            r.description = Core::GetStringValue(v, "description");
            r.userIds = Core::GetStringArrayValue(v, "userIds");
            r.createdAt = Core::GetStringValue(v, "createdAt");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, UserGroup const &obj) {
            jv = {
                    {"name", obj.name},
                    {"ern", obj.ern},
                    {"accountId", obj.accountId},
                    {"region", obj.region},
                    {"description", obj.description},
                    {"userIds", boost::json::value_from(obj.userIds)},
                    {"createdAt", obj.createdAt},
            };
        }
    };
}
