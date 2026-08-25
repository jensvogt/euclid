#pragma once

// C++ includes
#include <string>

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto {

    struct Account {

        /**
         * @brief Account ID, unique across the deployment
         */
        std::string accountId;

        /**
         * @brief Human-readable account name
         */
        std::string name;

        /**
         * @brief Euclid resource name
         */
        std::string ern;

        /**
         * @brief Free-text description of the account's purpose
         */
        std::string description;

        /**
         * @brief Creation timestamp
         */
        std::chrono::system_clock::time_point created;

        /**
         * @brief Modification timestamp
         */
        std::chrono::system_clock::time_point modified;

    private:

        friend Account tag_invoke(boost::json::value_to_tag<Account>, boost::json::value const &v) {
            Account r;
            r.accountId = Core::GetStringValue(v, "accountId");
            r.name = Core::GetStringValue(v, "name");
            r.ern = Core::GetStringValue(v, "ern");
            r.description = Core::GetStringValue(v, "description");
            r.created = Core::GetDatetimeValue(v, "created");
            r.modified = Core::GetDatetimeValue(v, "modified");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, Account const &obj) {
            jv = {
                    {"accountId", obj.accountId},
                    {"name", obj.name},
                    {"ern", obj.ern},
                    {"description", obj.description},
                    {"created", Core::DateTimeUtils::ToISO8601(obj.created)},
                    {"modified", Core::DateTimeUtils::ToISO8601(obj.modified)},
            };
        }
    };
}
