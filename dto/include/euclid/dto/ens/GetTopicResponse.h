// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/19/26.
//

#pragma once

// C++ includes
#include <string>

// C++ includes
#include <array>
#include <string_view>

// Euclid includes
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/ens/model/Topic.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::ENS {

    using Json::tag_invoke;

    /**
     * @brief One topic, as list-topics describes each of its own.
     *
     * @par
     * The same Topic model rather than a shape of its own, so that what a listing shows and what
     * this shows cannot drift apart - a field added to one is in the other by construction.
     */
    struct GetTopicResponse : BaseDto {

        /**
         * @brief The topic.
         */
        Topic topic;

        /**
         * @brief The members that must be in the JSON: missing or null, reading throws.
         */
        static constexpr std::array<std::string_view, 1> kJsonRequired{"topic"};

        /**
         * @brief Serializes this response to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(GetTopicResponse, (BaseDto), (topic))
}// namespace Euclid::Dto::ENS
