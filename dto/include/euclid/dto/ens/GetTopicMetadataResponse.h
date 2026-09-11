// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ENS {

    struct GetTopicMetadataResponse {

        /**
         * @brief Region of the queue
         */
        std::string region{};

        /**
         * @brief Account ID of the queue
         */
        std::string accountId{};

        /**
         * @brief Owner of the queue
         */
        std::string owner{};

        /**
         * @brief Namespace of queue
         */
        std::string nameSpace{};

        /**
         * @brief Queue name
         */
        std::string name{};

        /**
         * @brief Queue ERN
         */
        std::string ern{};

        /**
         * @brief Queue size in bytes
         */
        long size{};

        /**
         * @brief Total number of messages
         */
        long messages{};

        /**
         * @brief Whether the topic is delivering: RUNNING, or STOPPED while it holds what is
         * published to it - see euclid-cli-ens-stop-topic(1).
         */
        std::string status{"RUNNING"};

        /**
         * @brief How long a published message is kept, in seconds. Zero means the topic follows
         * euclid.modules.ens.retention-period.
         */
        long retentionPeriod{};

        /**
         * @brief How many messages are held, waiting for the topic to be started again.
         */
        long held{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend GetTopicMetadataResponse tag_invoke(boost::json::value_to_tag<GetTopicMetadataResponse>, boost::json::value const &v) {
            GetTopicMetadataResponse r;
            r.region = Core::GetStringValue(v, "region");
            r.accountId = Core::GetStringValue(v, "accountId");
            r.owner = Core::GetStringValue(v, "owner");
            r.nameSpace = Core::GetStringValue(v, "nameSpace");
            r.name = Core::GetStringValue(v, "name");
            r.ern = Core::GetStringValue(v, "ern");
            r.status = Core::GetStringValue(v, "status");
            r.retentionPeriod = Core::GetLongValue(v, "retentionPeriod", 0);
            r.held = Core::GetLongValue(v, "held", 0);
            r.size = Core::GetLongValue(v, "size");
            r.messages = Core::GetLongValue(v, "messages");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, GetTopicMetadataResponse const &obj) {
            jv = {
                    {"region", obj.region},
                    {"accountId", obj.accountId},
                    {"owner", obj.owner},
                    {"nameSpace", obj.nameSpace},
                    {"name", obj.name},
                    {"ern", obj.ern},
                    {"size", obj.size},
                    {"messages", obj.messages},
                    {"status", obj.status},
                    {"retentionPeriod", obj.retentionPeriod},
                    {"held", obj.held},
            };
        }
    };

}