// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/27/26.
//

#pragma once

#include <vector>

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::ESM {

    using Json::tag_invoke;

    struct SubscribeRequest {

        /**
         * @brief ERN of the bucket objects are created in.
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
         * @brief Which object events to deliver, empty for all of them: "esm.object.created",
         * "esm.object.updated", "esm.object.deleted".
         */
        std::vector<std::string> eventTypes;

        /**
         * @brief Only deliver objects whose key starts with this, empty for the whole bucket.
         */
        std::string prefix;

        /**
         * @brief Whether directory markers - the zero-byte objects whose key ends in "/" - count
         * as objects worth delivering. Off by default: most subscribers want files.
         */
        bool directories = false;

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

    BOOST_DESCRIBE_STRUCT(SubscribeRequest, (), (sourceErn, type, targetErn, eventTypes, prefix, directories))
}
