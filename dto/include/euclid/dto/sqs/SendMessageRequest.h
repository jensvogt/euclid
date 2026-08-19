//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <map>
#include <string>

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/sqs/model/Variant.h>

namespace Euclid::Dto::SQS {

    struct SendMessageRequest {

        /**
         * @brief Queue ERN
         */
        std::string queueErn{};

        /**
         * @brief Message body
         */
        std::string body{};

        /**
         * @brief Message attributes
         */
        std::map<std::string, Variant> attributes{};

        /**
         * @brief Message priority, i.e. "LOW", "MIDDLE" or "HIGH". Defaults to "MIDDLE".
         */
        std::string priority{"MIDDLE"};

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
        static SendMessageRequest fromJson(const std::string &json) {
            return boost::json::value_to<SendMessageRequest>(Core::ParseJsonString(json));
        }

    private:

        friend SendMessageRequest tag_invoke(boost::json::value_to_tag<SendMessageRequest>, boost::json::value const &v) {
            SendMessageRequest r;
            r.queueErn = Core::GetStringValue(v, "ern");
            r.body = Core::GetStringValue(v, "body");
            r.attributes = Core::GetMapFromObject<std::string, Variant>(v, "attributes");
            r.priority = Core::GetStringValue(v, "priority");
            if (r.priority.empty()) r.priority = "MIDDLE";
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, SendMessageRequest const &obj) {
            jv = {
                    {"ern", obj.queueErn},
                    {"body", obj.body},
                    {"attributes", boost::json::value_from(obj.attributes)},
                    {"priority", obj.priority},
            };
        }
    };

}
