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

    using std::chrono::system_clock;

    struct Topic {

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
         * @brief Queue size in bytes
         */
        long size = 0;

        /**
         * @brief Number of messages in the queue
         */
        long messages = 0;

        /**
         * @brief Visibility in seconds
         */
        long maxMessageLength = 1024 * 1024;

        /**
         * @brief How many messages have ever been published to this topic.
         *
         * A lifetime total rather than a current figure: unlike "messages", it does not fall when
         * the topic is purged or when retention removes what it was holding, which is what makes it
         * the one to read when asking whether a publisher is working at all.
         */
        long send = 0;

        /**
         * @brief How many messages have ever been handed to the subscriptions again.
         *
         * Incremented by resend-messages, and a lifetime total for the same reason as "send". A
         * topic with a resend count well above zero is one somebody has had to replay, which is
         * worth seeing next to what it has delivered.
         */
        long resend = 0;

        /**
         * @brief Whether the topic is delivering: RUNNING, or STOPPED while it holds what is
         * published to it - see euclid-cli-ens-stop-topic(1).
         */
        std::string status = "RUNNING";

        /**
         * @brief How long a message published to this topic is kept, in seconds. Zero means the
         * topic follows the installation's own setting; -1 means it keeps everything.
         */
        long retentionPeriod = 0;

        /**
         * @brief Whether a message is deleted once it has been handed to every subscription, rather
         * than kept for the retention period - see euclid-cli-ens-set-topic-delete-after-delivery(1).
         */
        bool deleteAfterDelivery = false;

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
        [[nodiscard]]
        static Topic fromJson(const std::string &json) {
            return boost::json::value_to<Topic>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(Topic, (), (name, owner, ern, tags, size, messages, maxMessageLength, send, resend, status, retentionPeriod, deleteAfterDelivery, created, modified))

}// namespace Euclid::Dto::ENS