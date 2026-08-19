//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/sqs/model/Variant.h>

namespace Euclid::Dto::SQS {

    using std::chrono::system_clock;

    struct Message {

        /**
         * @brief Euclid queue resource name
         */
        std::string ern;

        /**
         * @brief Euclid queue resource name
         */
        std::string queueErn;

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
         * @brief MD5 sum of the body
         */
        std::string md5Body;

        /**
         * @brief Receipt handle, required to delete the message after receiving it
         */
        std::string receiptHandle;

        /**
         * @brief Message attributes
         */
        std::map<std::string, Variant> attributes;

        /**
         * @brief MD5 sum of the message attributes
         */
        std::string md5Attributes;

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
            r.queueErn = Core::GetStringValue(v, "queueErn");
            r.messageId = Core::GetStringValue(v, "messageId");
            r.status = Core::GetStringValue(v, "status");
            r.body = Core::GetStringValue(v, "body");
            r.md5Body = Core::GetStringValue(v, "md5sum");
            r.receiptHandle = Core::GetStringValue(v, "receiptHandle");
            r.attributes = Core::GetMapFromObject<std::string, Variant>(v, "attributes");
            r.md5Attributes = Core::GetStringValue(v, "md5Attributes");
            r.lastReceived = Core::GetDatetimeValue(v, "lastReceived");
            r.created = Core::GetDatetimeValue(v, "created");
            r.modified = Core::GetDatetimeValue(v, "modified");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, Message const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"queueErn", obj.queueErn},
                    {"messageId", obj.messageId},
                    {"status", obj.status},
                    {"body", obj.body},
                    {"md5sum", obj.md5Body},
                    {"receiptHandle", obj.receiptHandle},
                    {"attributes", boost::json::value_from(obj.attributes)},
                    {"md5Attributes", obj.md5Attributes},
                    {"lastReceived", Core::DateTimeUtils::ToISO8601(obj.lastReceived)},
                    {"created", Core::DateTimeUtils::ToISO8601(obj.created)},
                    {"modified", Core::DateTimeUtils::ToISO8601(obj.modified)},
            };
        }
    };

}// namespace Euclid::Dto::SQS