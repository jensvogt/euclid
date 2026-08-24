//
// Created by vogje01 on 8/19/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EQS {

    struct GetMessageMetadataResponse {

        /**
         * @brief Message ID
         */
        std::string messageId{};

        /**
         * @brief ERN of the queue this message belongs to
         */
        std::string queueErn{};

        /**
         * @brief Receipt handle, if the message is currently in flight
         */
        std::string receiptHandle{};

        /**
         * @brief Message status, i.e. "AVAILABLE", "DELAYED", "INVISIBLE" or "UNKNOWN"
         */
        std::string status{};

        /**
         * @brief Message priority, i.e. "LOW", "MIDDLE" or "HIGH"
         */
        std::string priority{};

        /**
         * @brief Message size in bytes
         */
        long size{};

        /**
         * @brief Number of times this message has been received (ApproximateReceiveCount)
         */
        long receivedCount{};

        /**
         * @brief Visibility timeout in seconds
         */
        long visibilityTimeout{};

        /**
         * @brief Content type
         */
        std::string contentType{};

        /**
         * @brief MD5 sum of the message body
         */
        std::string md5Body{};

        /**
         * @brief MD5 sum of the message attributes
         */
        std::string md5Attributes{};

        /**
         * @brief Creation date, ISO8601
         */
        std::string created{};

        /**
         * @brief Last modification date, ISO8601
         */
        std::string modified{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend GetMessageMetadataResponse tag_invoke(boost::json::value_to_tag<GetMessageMetadataResponse>, boost::json::value const &v) {
            GetMessageMetadataResponse r;
            r.messageId = Core::GetStringValue(v, "messageId");
            r.queueErn = Core::GetStringValue(v, "queueErn");
            r.receiptHandle = Core::GetStringValue(v, "receiptHandle");
            r.status = Core::GetStringValue(v, "status");
            r.priority = Core::GetStringValue(v, "priority");
            r.size = Core::GetLongValue(v, "size");
            r.receivedCount = Core::GetLongValue(v, "receivedCount");
            r.visibilityTimeout = Core::GetLongValue(v, "visibilityTimeout");
            r.contentType = Core::GetStringValue(v, "contentType");
            r.md5Body = Core::GetStringValue(v, "md5Body");
            r.md5Attributes = Core::GetStringValue(v, "md5Attributes");
            r.created = Core::GetStringValue(v, "created");
            r.modified = Core::GetStringValue(v, "modified");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, GetMessageMetadataResponse const &obj) {
            jv = {
                    {"messageId", obj.messageId},
                    {"queueErn", obj.queueErn},
                    {"receiptHandle", obj.receiptHandle},
                    {"status", obj.status},
                    {"priority", obj.priority},
                    {"size", obj.size},
                    {"receivedCount", obj.receivedCount},
                    {"visibilityTimeout", obj.visibilityTimeout},
                    {"contentType", obj.contentType},
                    {"md5Body", obj.md5Body},
                    {"md5Attributes", obj.md5Attributes},
                    {"created", obj.created},
                    {"modified", obj.modified},
            };
        }
    };

}
