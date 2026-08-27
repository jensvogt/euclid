//
// Created by vogje01 on 8/27/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ENS {

    struct SubscribeRequest {

        /**
         * @brief ERN of the topic messages are published to.
         */
        std::string sourceErn;

        /**
         * @brief Delivery protocol. Only "SQS" is supported for now.
         */
        std::string type;

        /**
         * @brief ERN of the delivery target. For type "SQS", an EQS queue ERN.
         */
        std::string targetErn;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static SubscribeRequest fromJson(const std::string &json) {
            return boost::json::value_to<SubscribeRequest>(Core::ParseJsonString(json));
        }

    private:

        friend SubscribeRequest tag_invoke(boost::json::value_to_tag<SubscribeRequest>, boost::json::value const &v) {
            SubscribeRequest r;
            r.sourceErn = Core::GetStringValue(v, "sourceErn");
            r.type = Core::GetStringValue(v, "type");
            r.targetErn = Core::GetStringValue(v, "targetErn");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, SubscribeRequest const &obj) {
            jv = {
                    {"sourceErn", obj.sourceErn},
                    {"type", obj.type},
                    {"targetErn", obj.targetErn},
            };
        }
    };
}
