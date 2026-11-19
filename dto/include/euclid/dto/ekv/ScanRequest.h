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
     * @brief Reads a table's items without regard to their key.
     */
    struct ScanRequest : BaseDto {

        /**
         * @brief Table to scan.
         */
        std::string table;

        /**
         * @brief Most to return; 0 means no limit.
         */
        long pageSize{0};

        /**
         * @brief Zero-based page, applied when pageSize is set.
         */
        long pageIndex{0};

        /**
         * @brief What to order by: "pk", "sk" or "modified". Anything else, the empty string
         * included, is key order - which is what a caller that does not care sends.
         *
         * @par
         * Only the columns stored beside the item, never one of its attributes: an attribute is a
         * field nothing indexes, on a collection a scan is already the expensive way to read.
         */
        std::string sortColumn;

        /**
         * @brief "desc" for descending; anything else ascends.
         */
        std::string sortDirection;

        /**
         * @brief ISO-8601 bounds on when an item was last written. Either may be empty.
         *
         * @par
         * The question a query cannot answer: it is bounded by a partition, and "what changed
         * since Friday" names none.
         */
        std::string modifiedFrom;
        std::string modifiedTo;

        /**
         * @brief An attribute to compare, or empty for none. Any attribute, not only a key one -
         * reaching a key is what a query is for.
         */
        std::string attributeName;

        /**
         * @brief How to compare it: eq, lt, le, gt, ge, between or begins-with, in the vocabulary
         * a query uses for the sort key. Empty narrows nothing.
         */
        std::string attributeOperator;

        /**
         * @brief What to compare against, and the upper bound for "between". Typed as the value
         * is written, so a number compares as a number.
         */
        boost::json::value attributeValue;
        boost::json::value attributeUpper;

        /**
         * @brief Serializes this to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(ScanRequest, (BaseDto), (table, pageSize, pageIndex, sortColumn, sortDirection,
                                                    modifiedFrom, modifiedTo, attributeName, attributeOperator,
                                                    attributeValue, attributeUpper))
}
