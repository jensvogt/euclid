// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/19/26.
//

#pragma once

// C++ includes
#include <string>

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::ENS {

    using Json::tag_invoke;

    /**
     * @brief Asks for one topic, by ERN or by name.
     *
     * @par
     * Two ways in because both are natural: an ERN is what every other call passes around, and a
     * name is what a person has. A name is resolved in the caller's own account and namespace, so
     * it means "my topic of that name" and cannot reach another account's topic called the
     * same thing - the same rule get-topic-ern applies.
     */
    struct GetTopicRequest {

        /**
         * @brief Topic ERN, or empty when the topic is named instead.
         */
        std::string ern{};

        /**
         * @brief Topic name, used when no ERN is given.
         */
        std::string name{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(GetTopicRequest, (), (ern, name))
}// namespace Euclid::Dto::ENS
