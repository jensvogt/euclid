//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EQS {

    struct DeleteMessageRequest {

        /**
         * @brief Receipt handle of the message
         *
         * Required to delete a message that is currently in flight (status INVISIBLE). Mutually
         * exclusive with messageId; if both are set, receiptHandle takes precedence.
         */
        std::string receiptHandle;

        /**
         * @brief Message ID
         *
         * Alternative to receiptHandle, allowing a message to be deleted before it has ever been
         * received (e.g. status AVAILABLE or DELAYED), bypassing the usual lease/lock semantics.
         * This is a Euclid-specific extension; AWS SQS has no equivalent.
         */
        std::string messageId;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]]
        std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]]
        static DeleteMessageRequest fromJson(const std::string &json) {
            return boost::json::value_to<DeleteMessageRequest>(Core::ParseJsonString(json));
        }

    private:

        friend DeleteMessageRequest tag_invoke(boost::json::value_to_tag<DeleteMessageRequest>, boost::json::value const &v) {
            DeleteMessageRequest r;
            r.receiptHandle = Core::GetStringValue(v, "receiptHandle");
            r.messageId = Core::GetStringValue(v, "messageId");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, DeleteMessageRequest const &obj) {
            jv = {
                    {"receiptHandle", obj.receiptHandle},
                    {"messageId", obj.messageId},
            };
        }
    };
}