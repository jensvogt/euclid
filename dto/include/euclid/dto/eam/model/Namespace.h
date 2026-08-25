#pragma once

// C++ includes
#include <string>

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto {

    struct Namespace {

        /**
         * @brief Account this namespace belongs to
         */
        std::string accountId;

        /**
         * @brief Namespace name, unique within its account
         */
        std::string name;

        /**
         * @brief Euclid resource name
         */
        std::string ern;

        /**
         * @brief Free-text description of the namespace's purpose
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

        friend Namespace tag_invoke(boost::json::value_to_tag<Namespace>, boost::json::value const &v) {
            Namespace r;
            r.accountId = Core::GetStringValue(v, "accountId");
            r.name = Core::GetStringValue(v, "name");
            r.ern = Core::GetStringValue(v, "ern");
            r.description = Core::GetStringValue(v, "description");
            r.created = Core::GetDatetimeValue(v, "created");
            r.modified = Core::GetDatetimeValue(v, "modified");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, Namespace const &obj) {
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
