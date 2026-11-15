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

namespace Euclid::Dto::EQS {

    using Json::tag_invoke;

    struct CreateQueueRequest {

        /**
         * @brief Name of the queue
         */
        std::string name;

        /**
         * @brief Visibility in seconds
         */
        long visibility = 30;

        /**
         * @brief Maximal number of reties
         */
        long maxRetries = 3;

        /**
         * @brief Maximal length of a message in bytes
         */
        /**
         * @brief One mebibyte when the caller omits the field, rather than the zero a plain read
         * would give: a client that says nothing about the size of its messages means "the usual",
         * not "a queue that accepts nothing" - and since a send is measured against this, reading
         * an absent field as zero would refuse every message sent to such a queue.
         */
        long maxMessageLength = 1024 * 1024;

        /**
         * @brief Name of the dead letter queue. Can be empty.
         */
        std::string dlqName;

        /**
         * @brief Delay in seconds for any message send to this queue.
         */
        long delay = 0;

        /**
         * @brief Default priority for new messages
         */
        std::string priority = "MEDIUM";

        /**
         * @brief Whether this queue is euclid's own plumbing rather than a user's queue.
         *
         * @par
         * An internal queue is left out of list-queues and the queue count; it behaves like any
         * other in every other respect. For a delivery that has to land somewhere - a bucket
         * subscription's target, say - where the queue is an implementation detail of whoever
         * registered it. See Database::Entity::EQS::Queue::internal.
         */
        bool internal = false;

        /**
         * @brief The members that keep their initial value when the JSON leaves them out, rather than
         * becoming zero, as the other members do.
         */
        static constexpr std::array<std::string_view, 4> kJsonDefaults{"visibility", "maxRetries", "maxMessageLength", "priority"};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static CreateQueueRequest fromJson(const std::string &json) {
            return boost::json::value_to<CreateQueueRequest>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(CreateQueueRequest, (), (name, visibility, maxRetries, maxMessageLength, dlqName, delay, priority, internal))
}