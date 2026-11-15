// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/23/26.
//

#pragma once

// C++ includes
#include <string>
#include <vector>

// C++ includes
#include <array>
#include <string_view>

// Euclid includes
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/esm/model/Bucket.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::ESM {

    using Json::tag_invoke;

    struct ListBucketsResponse : BaseDto {

        /**
         * @brief Bucket list
         */
        std::vector<Bucket> buckets;

        /**
         * @brief total number of buckets
         */
        long total{};

        /**
         * @brief The members that must be in the JSON: missing or null, reading throws.
         */
        static constexpr std::array<std::string_view, 1> kJsonRequired{"buckets"};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(ListBucketsResponse, (BaseDto), (buckets, total))

}// namespace Euclid::Dto::ESM
