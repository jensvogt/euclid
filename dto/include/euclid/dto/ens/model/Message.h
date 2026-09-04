//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/com/Variant.h>

namespace Euclid::Dto::ENS {

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

    private:

        friend Message tag_invoke(boost::json::value_to_tag<Message>, boost::json::value const &v) {
            Message r;
            r.ern = Core::GetStringValue(v, "ern");
            r.topicErn = Core::GetStringValue(v, "topicErn");
            r.messageId = Core::GetStringValue(v, "messageId");
            r.status = Core::GetStringValue(v, "status");
            r.body = Core::GetStringValue(v, "body");
            r.attributes = Core::GetMapFromObject<std::string, COM::Variant>(v, "attributes");
            r.contentType = Core::GetStringValue(v, "contentType");
            r.lastReceived = Core::GetDatetimeValue(v, "lastReceived");
            r.created = Core::GetDatetimeValue(v, "created");
            r.modified = Core::GetDatetimeValue(v, "modified");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, Message const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"topicErn", obj.topicErn},
                    {"messageId", obj.messageId},
                    {"status", obj.status},
                    {"body", obj.body},
                    {"attributes", boost::json::value_from(obj.attributes)},
                    {"contentType", obj.contentType},
                    {"lastReceived", Core::DateTimeUtils::ToISO8601(obj.lastReceived)},
                    {"created", Core::DateTimeUtils::ToISO8601(obj.created)},
                    {"modified", Core::DateTimeUtils::ToISO8601(obj.modified)},
            };
        }
    };

}// namespace Euclid::Dto::EQS