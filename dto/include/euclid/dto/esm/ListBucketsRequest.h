// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/23/26.
//

#pragma once

// C++ includes
#include <string>

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::ESM {

    using Json::tag_invoke;

    struct ListBucketsRequest {

        /**
         * @brief Bucket name prefix
         */
        std::string prefix{};

        /**
         * @brief How prefix is matched: "prefix", "endsWith", "contains" or "regex"; empty means
         * "prefix". Every type but "regex" takes prefix literally.
         */
        std::string matchType{};

        /**
         * @brief Page size
         */
        long pageSize = 10;

        /**
         * @brief Page index
         */
        long pageIndex = 0;

        /**
         * @brief Sorting columns
         */
        std::string sortColumn = "name";

        /**
         * @brief Sort direction
         */
        std::string sortDirection = "asc";

        /**
         * @brief Whether euclid's own internal buckets are listed as well.
         *
         * Honoured for administrators only - see EsmServer's list-buckets handler. Off by default,
         * so every existing caller keeps the listing it had.
         */
        bool includeInternal = false;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(ListBucketsRequest, (), (prefix, matchType, pageSize, pageIndex, sortColumn, sortDirection, includeInternal))

}// namespace Euclid::Dto::ESM
