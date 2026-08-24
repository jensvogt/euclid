//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EQS {

    using std::chrono::system_clock;

    struct Queue {

        /**
         * @brief Region the queue lives in
         */
        std::string region;

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
        long messages = 0;

        /**
         * @brief Number of delayed messages in the queue
         */
        long delayed = 0;

        /**
         * @brief Number of busy messages in the queue
         */
        long busy = 0;

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
        static Queue fromJson(const std::string &json) {
            return boost::json::value_to<Queue>(Core::ParseJsonString(json));
        }

    private:

        friend Queue tag_invoke(boost::json::value_to_tag<Queue>, boost::json::value const &v) {
            Queue r;
            r.region = Core::GetStringValue(v, "region");
            r.name = Core::GetStringValue(v, "name");
            r.owner = Core::GetStringValue(v, "owner");
            r.ern = Core::GetStringValue(v, "ern");
            r.tags = Core::GetMapFromObject<std::string, std::string>(v, "tags");
            r.size = Core::GetLongValue(v, "size", 0);
            r.delay = Core::GetLongValue(v, "delay", 0);
            r.messages = Core::GetLongValue(v, "messages", 0);
            r.delayed = Core::GetLongValue(v, "delayed", 0);
            r.busy = Core::GetLongValue(v, "busy", 0);
            r.visibility = Core::GetLongValue(v, "visibility", 30);
            r.maxMessageLength = Core::GetLongValue(v, "maxMessageLength", 1024 * 1024);
            r.maxReceiveCount = Core::GetLongValue(v, "maxReceiveCount", 3);
            r.deadLetterQueueArn = Core::GetStringValue(v, "deadLetterQueueArn");
            r.created = Core::GetDatetimeValue(v, "created");
            r.modified = Core::GetDatetimeValue(v, "modified");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, Queue const &obj) {
            jv = {
                    {"region", obj.region},
                    {"name", obj.name},
                    {"owner", obj.owner},
                    {"ern", obj.ern},
                    {"tags", boost::json::value_from(obj.tags)},
                    {"size", obj.size},
                    {"delay", obj.delay},
                    {"messages", obj.messages},
                    {"delayed", obj.delayed},
                    {"busy", obj.busy},
                    {"visibility", obj.visibility},
                    {"maxMessageLength", obj.maxMessageLength},
                    {"maxReceiveCount", obj.maxReceiveCount},
                    {"deadLetterQueueArn", obj.deadLetterQueueArn},
                    {"created", Core::DateTimeUtils::ToISO8601(obj.created)},
                    {"modified", Core::DateTimeUtils::ToISO8601(obj.modified)},
            };
        }
    };

}// namespace Euclid::Dto::EQS