// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::ESM {

    using Json::tag_invoke;

    /**
     * @brief How many objects a bucket holds under a prefix, counted at the moment of asking.
     */
    struct CountObjectsResponse : BaseDto {

        /**
         * @brief Bucket ERN
         */
        std::string ern{};

        /**
         * @brief The prefix this count was taken under, echoed so an answer is readable on its
         * own - a bare number does not say what it counted.
         */
        std::string prefix{};

        /**
         * @brief Whether directory markers were counted.
         */
        bool includeDirectories{false};

        /**
         * @brief Object count
         */
        long count{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(CountObjectsResponse, (BaseDto), (ern, prefix, includeDirectories, count))

}// namespace Euclid::Dto::ESM
