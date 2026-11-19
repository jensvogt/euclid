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
        std::string partitionKey;

        /**
         * @brief What type that attribute has: string, number or binary.
         */
        std::string partitionKeyType;

        /**
         * @brief Attribute items within a partition are ordered by, or empty for none.
         */
        std::string sortKey;

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
    };

    BOOST_DESCRIBE_STRUCT(CreateTableRequest, (BaseDto), (name, partitionKey, partitionKeyType, sortKey, sortKeyType))
}
