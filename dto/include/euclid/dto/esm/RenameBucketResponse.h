// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/2/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::ESM {

    using Json::tag_invoke;

    /**
     * @brief The bucket as it is now, and how much had to be rewritten to get there.
     */
    struct RenameBucketResponse : BaseDto {

        /**
         * @brief New name of the bucket
         */
        std::string name;

        /**
         * @brief New ERN of the bucket - it carries the name, so it changed too
         */
        std::string ern;

        /**
         * @brief Number of objects whose bucket reference and ERN were rewritten
         */
        long objects = 0;

        /**
         * @brief Number of bucket subscriptions repointed at the new ERN
         */
        long subscriptions = 0;

        /**
         * @brief Serializes this response to a JSON string
         */
        [[nodiscard]]
        std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(RenameBucketResponse, (BaseDto), (name, ern, objects, subscriptions))

}// namespace Euclid::Dto::ESM
