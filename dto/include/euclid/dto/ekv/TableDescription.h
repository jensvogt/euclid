// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

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
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::EKV {

    using Json::tag_invoke;

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
        std::string partitionKey;

        /**
         * @brief Its type.
         */
        std::string partitionKeyType;

        /**
         * @brief Sort key attribute, empty when the table has none.
         */
        std::string sortKey;

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
    };

    BOOST_DESCRIBE_STRUCT(TableDescription, (BaseDto), (name, ern, partitionKey, partitionKeyType, sortKey, sortKeyType, itemCount, created, modified))
}
