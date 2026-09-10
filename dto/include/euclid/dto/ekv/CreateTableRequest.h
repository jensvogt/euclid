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
     * @brief Creates a table with the key its items are identified by.
     */
    struct CreateTableRequest : BaseDto {

        /**
         * @brief Table name, unique within the account.
         */
        std::string name;

        /**
         * @brief Attribute every item is identified by.
         */
        std::string partitionKeyName;

        /**
         * @brief What type that attribute has: string, number or binary.
         */
        std::string partitionKeyType;

        /**
         * @brief Attribute items within a partition are ordered by, or empty for none.
         */
        std::string sortKeyName;

        /**
         * @brief What type the sort key has; ignored when there is no sort key.
         */
        std::string sortKeyType;

        /**
         * @brief Serializes this to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend CreateTableRequest tag_invoke(boost::json::value_to_tag<CreateTableRequest>, boost::json::value const &v) {
            CreateTableRequest r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.name = Core::GetStringValue(v, "name");
            r.partitionKeyName = Core::GetStringValue(v, "partitionKey");
            r.partitionKeyType = Core::GetStringValue(v, "partitionKeyType");
            r.sortKeyName = Core::GetStringValue(v, "sortKey");
            r.sortKeyType = Core::GetStringValue(v, "sortKeyType");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, CreateTableRequest const &obj) {
            jv = {
                    {"metadata", boost::json::value_from(static_cast<const BaseDto &>(obj))},
                    {"name", obj.name},
                    {"partitionKey", obj.partitionKeyName},
                    {"partitionKeyType", obj.partitionKeyType},
                    {"sortKey", obj.sortKeyName},
                    {"sortKeyType", obj.sortKeyType},
            };
        }
    };
}
