// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::EQS {

    using Json::tag_invoke;

    struct DeleteMessageRequest {

        /**
         * @brief Receipt handle of the message
         *
         * Required to delete a message that is currently in flight (status INVISIBLE). Mutually
         * exclusive with messageId; if both are set, receiptHandle takes precedence.
         */
        std::string receiptHandle;

        /**
         * @brief Message ID
         *
         * Alternative to receiptHandle, allowing a message to be deleted before it has ever been
         * received (e.g. status AVAILABLE or DELAYED), bypassing the usual lease/lock semantics.
         * This is a Euclid-specific extension; AWS SQS has no equivalent.
         */
        std::string messageId;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]]
        std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]]
        static DeleteMessageRequest fromJson(const std::string &json) {
            return boost::json::value_to<DeleteMessageRequest>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(DeleteMessageRequest, (), (receiptHandle, messageId))
}