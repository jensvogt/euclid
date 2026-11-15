// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/19/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::EQS {

    using Json::tag_invoke;

    struct GetMessageMetadataResponse : BaseDto {

        /**
         * @brief Message ID
         */
        std::string messageId{};

        /**
         * @brief ERN of the queue this message belongs to
         */
        std::string queueErn{};

        /**
         * @brief Receipt handle, if the message is currently in flight
         */
        std::string receiptHandle{};

        /**
         * @brief Message status, i.e. "AVAILABLE", "DELAYED", "INVISIBLE" or "UNKNOWN"
         */
        std::string status{};

        /**
         * @brief Message priority, i.e. "LOW", "MEDIUM" or "HIGH"
         */
        std::string priority{};

        /**
         * @brief Message size in bytes
         */
        long size{};

        /**
         * @brief Number of times this message has been received (ApproximateReceiveCount)
         */
        long receivedCount{};

        /**
         * @brief Visibility timeout in seconds
         */
        long visibilityTimeout{};

        /**
         * @brief Content type
         */
        std::string contentType{};

        /**
         * @brief Creation date, ISO8601
         */
        std::string created{};

        /**
         * @brief Last modification date, ISO8601
         */
        std::string modified{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(GetMessageMetadataResponse, (BaseDto), (messageId, queueErn, receiptHandle, status, priority, size, receivedCount, visibilityTimeout, contentType, created, modified))

}
