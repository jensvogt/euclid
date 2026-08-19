//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::SQS {

    struct GetQueueMetadataResponse {

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
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend GetQueueMetadataResponse tag_invoke(boost::json::value_to_tag<GetQueueMetadataResponse>, boost::json::value const &v) {
            GetQueueMetadataResponse r;
            r.region = Core::GetStringValue(v, "region");
            r.accountId = Core::GetStringValue(v, "accountId");
            r.owner = Core::GetStringValue(v, "owner");
            r.nameSpace = Core::GetStringValue(v, "nameSpace");
            r.name = Core::GetStringValue(v, "name");
            r.ern = Core::GetStringValue(v, "ern");
            r.size = Core::GetLongValue(v, "size");
            r.messages = Core::GetLongValue(v, "messages");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, GetQueueMetadataResponse const &obj) {
            jv = {
                    {"region", obj.region},
                    {"accountId", obj.accountId},
                    {"owner", obj.owner},
                    {"nameSpace", obj.nameSpace},
                    {"name", obj.name},
                    {"ern", obj.ern},
                    {"size", obj.size},
                    {"messages", obj.messages},
            };
        }
    };

}