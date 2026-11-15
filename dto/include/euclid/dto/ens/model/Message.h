// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/com/Variant.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::ENS {

    using Json::tag_invoke;

    using std::chrono::system_clock;

    struct Message {

        /**
         * @brief Euclid queue resource name
         */
        std::string ern;

        /**
         * @brief Euclid topic resource name
         */
        std::string topicErn;

        /**
         * @brief Message ID
         */
        std::string messageId;

        /**
         * @brief Message status, i.e. "AVAILABLE", "DELAYED", "INVISIBLE" or "UNKNOWN"
         */
        std::string status;

        /**
         * @brief Message body
         */
        std::string body;

        /**
         * @brief Message attributes
         */
        std::map<std::string, COM::Variant> attributes;

        /**
         * @brief Content type
         */
        std::string contentType;

        /**
         * @brief Last received timestamp
         */
        system_clock::time_point lastReceived;

        /**
         * @brief Creation date
         */
        system_clock::time_point created;

        /**
         * @brief Last modification date
         */
        system_clock::time_point modified;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]]
        static Message fromJson(const std::string &json) {
            return boost::json::value_to<Message>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(Message, (), (ern, topicErn, messageId, status, body, attributes, contentType, lastReceived, created, modified))

}// namespace Euclid::Dto::ENS