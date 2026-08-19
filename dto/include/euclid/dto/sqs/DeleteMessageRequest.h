//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::SQS {

    struct DeleteMessageRequest {

        /**
         * @brief Receipt handle of the message
         */
        std::string receiptHandle;

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
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, DeleteMessageRequest const &obj) {
            jv = {
                    {"receiptHandle", obj.receiptHandle},
            };
        }
    };
}