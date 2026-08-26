//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ENS {

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
        static Topic fromJson(const std::string &json) {
            return boost::json::value_to<Topic>(Core::ParseJsonString(json));
        }

    private:

        friend Topic tag_invoke(boost::json::value_to_tag<Topic>, boost::json::value const &v) {
            Topic r;
            r.name = Core::GetStringValue(v, "name");
            r.owner = Core::GetStringValue(v, "owner");
            r.ern = Core::GetStringValue(v, "ern");
            r.tags = Core::GetMapFromObject<std::string, std::string>(v, "tags");
            r.size = Core::GetLongValue(v, "size", 0);
            r.messages = Core::GetLongValue(v, "messages", 0);
            r.maxMessageLength = Core::GetLongValue(v, "maxMessageLength", 1024 * 1024);
            r.created = Core::GetDatetimeValue(v, "created");
            r.modified = Core::GetDatetimeValue(v, "modified");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, Topic const &obj) {
            jv = {
                    {"name", obj.name},
                    {"owner", obj.owner},
                    {"ern", obj.ern},
                    {"tags", boost::json::value_from(obj.tags)},
                    {"size", obj.size},
                    {"messages", obj.messages},
                    {"maxMessageLength", obj.maxMessageLength},
                    {"created", Core::DateTimeUtils::ToISO8601(obj.created)},
                    {"modified", Core::DateTimeUtils::ToISO8601(obj.modified)},
            };
        }
    };

}// namespace Euclid::Dto::EQS