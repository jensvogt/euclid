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

    using std::chrono::system_clock;

    struct Queue {

        /**
         * @brief Queue name
         */
        std::string name;

        /**
         * @brief Queue owner
         */
        std::string owner;

        /**
         * @brief Euclid resource name
         */
        std::string ern;

        /**
         * @brief Queue tags
         */
        std::map<std::string, std::string> tags;

        /**
         * @brief Queue message delay in seconds
         */
        long delay = 0;

        /**
         * @brief Queue size in bytes
         */
        long size = 0;

        /**
         * @brief Number of messages in the queue
         */
        long available = 0;

        /**
         * @brief Number of delayed messages in the queue
         */
        long delayed = 0;

        /**
         * @brief Number of busy messages in the queue
         */
        long invisible = 0;

        /**
         * @brief Visibility in seconds
         */
        long visibility = 30;

        /**
         * @brief Visibility in seconds
         */
        long maxMessageLength = 1024 * 1024;

        /**
         * @brief Number of times a message can be received before it is moved to the dead-letter queue
         */
        long maxReceiveCount = 3;

        /**
         * @brief Number of times a message can be received before it is moved to the dead-letter queue
         */
        std::string deadLetterQueueArn;

        /**
         * @brief Default priority for new messages
         */
        std::string priority;

        /**
         * @brief Whether this queue may be received from: "AVAILABLE" or "STOPPED". Sending is
         * unaffected either way.
         */
        std::string status;

        /**
         * @brief Whether this is one of euclid's own queues rather than the caller's.
         *
         * Left out of list-queues unless an administrator asks for it, and reported here so a
         * listing that does include them can say which rows they are. See
         * Database::Entity::EQS::Queue::internal.
         */
        bool internal = false;

        /**
         * @brief Creation date
         */
        system_clock::time_point created;

        /**
         * @brief Last modification date
         */
        system_clock::time_point modified;

        /**
         * @brief The members that keep their initial value when the JSON leaves them out, rather than
         * becoming zero, as the other members do.
         */
        static constexpr std::array<std::string_view, 3> kJsonDefaults{"visibility", "maxMessageLength", "maxReceiveCount"};

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
        static Queue fromJson(const std::string &json) {
            return boost::json::value_to<Queue>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(Queue, (), (name, owner, ern, tags, delay, size, available, delayed, invisible, visibility, maxMessageLength, maxReceiveCount, deadLetterQueueArn, priority, status, internal, created, modified))

}// namespace Euclid::Dto::EQS