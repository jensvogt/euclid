//
// Created by vogje01 on 9/10/26.
//

#pragma once

// C++ includes
#include <string>
#include <vector>

// Boost includes
#include <boost/json.hpp>

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>

namespace Euclid::Dto::EKV {

    /**
     * @brief What a table looks like, as an answer.
     */
    struct TableDescription : BaseDto {

        /**
         * @brief Table name.
         */
        std::string name;

        /**
         * @brief Euclid resource name.
         */
        std::string ern;

        /**
         * @brief Attribute every item is identified by.
         */
        std::string partitionKeyName;

        /**
         * @brief Its type.
         */
        std::string partitionKeyType;

        /**
         * @brief Sort key attribute, empty when the table has none.
         */
        std::string sortKeyName;

        /**
         * @brief Its type, empty when the table has no sort key.
         */
        std::string sortKeyType;

        /**
         * @brief How many items the table holds, counted when asked.
         */
        long itemCount{0};

        /**
         * @brief Creation timestamp, ISO8601.
         */
        std::string created;

        /**
         * @brief Modification timestamp, ISO8601.
         */
        std::string modified;

        /**
         * @brief Serializes this to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend TableDescription tag_invoke(boost::json::value_to_tag<TableDescription>, boost::json::value const &v) {
            TableDescription r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.name = Core::GetStringValue(v, "name");
            r.ern = Core::GetStringValue(v, "ern");
            r.partitionKeyName = Core::GetStringValue(v, "partitionKey");
            r.partitionKeyType = Core::GetStringValue(v, "partitionKeyType");
            r.sortKeyName = Core::GetStringValue(v, "sortKey");
            r.sortKeyType = Core::GetStringValue(v, "sortKeyType");
            r.itemCount = Core::GetLongValue(v, "itemCount");
            r.created = Core::GetStringValue(v, "created");
            r.modified = Core::GetStringValue(v, "modified");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, TableDescription const &obj) {
            jv = {
                    {"metadata", boost::json::value_from(static_cast<const BaseDto &>(obj))},
                    {"name", obj.name},
                    {"ern", obj.ern},
                    {"partitionKey", obj.partitionKeyName},
                    {"partitionKeyType", obj.partitionKeyType},
                    {"sortKey", obj.sortKeyName},
                    {"sortKeyType", obj.sortKeyType},
                    {"itemCount", obj.itemCount},
                    {"created", obj.created},
                    {"modified", obj.modified},
            };
        }
    };
}
