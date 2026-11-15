// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/27/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::ENS {

    using Json::tag_invoke;

    struct SubscribeRequest {

        /**
         * @brief ERN of the topic messages are published to.
         */
        std::string sourceErn;

        /**
         * @brief Delivery protocol. Only "SQS" is supported for now.
         */
        std::string type;

        /**
         * @brief ERN of the delivery target. For type "SQS", an EQS queue ERN.
         */
        std::string targetErn;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static SubscribeRequest fromJson(const std::string &json) {
            return boost::json::value_to<SubscribeRequest>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(SubscribeRequest, (), (sourceErn, type, targetErn))
}
