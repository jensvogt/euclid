// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <array>
#include <string_view>

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::ENS {

    using Json::tag_invoke;

    struct CreateTopicRequest {

        /**
         * @brief Name of the queue
         */
        std::string name;

        /**
         * @brief Maximal length of a message in bytes.
         *
         * @par
         * One mebibyte when the caller omits the field, rather than the zero a plain read would
         * give: a client that says nothing about the size of its messages means "the usual", not
         * "a topic that accepts nothing" - and since a publish is measured against this, reading
         * an absent field as zero would refuse every message sent to such a topic.
         */
        long maxMessageLength = 1024 * 1024;

        /**
         * @brief The members that keep their initial value when the JSON leaves them out, rather than
         * becoming zero, as the other members do.
         */
        static constexpr std::array<std::string_view, 1> kJsonDefaults{"maxMessageLength"};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static CreateTopicRequest fromJson(const std::string &json) {
            return boost::json::value_to<CreateTopicRequest>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(CreateTopicRequest, (), (name, maxMessageLength))
}