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
     * @brief Reads the items of one partition, in sort-key order.
     */
    struct QueryRequest : BaseDto {

        /**
         * @brief Table to query.
         */
        std::string table;

        /**
         * @brief The partition key's value.
         */
        boost::json::value partitionKey;

        /**
         * @brief How to narrow by sort key: eq, lt, le, gt, ge, between, begins-with; empty takes the whole partition.
         */
        std::string sortOperator;

        /**
         * @brief What to compare the sort key against; the lower bound for between.
         */
        boost::json::value sortValue;

        /**
         * @brief The upper bound, for between only.
         */
        boost::json::value sortUpper;

        /**
         * @brief Whether to read in ascending sort-key order.
         */
        bool forward{true};

        /**
         * @brief Most to return; 0 means no limit.
         */
        long pageSize{0};

        /**
         * @brief Zero-based page, applied when pageSize is set.
         */
        long pageIndex{0};

        /**
         * @brief Serializes this to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(QueryRequest, (BaseDto), (table, partitionKey, sortOperator, sortValue, sortUpper, forward, pageSize, pageIndex))
}
