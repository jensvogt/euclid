//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EQS {

    struct SetQueueTagRequest {

        /**
         * @brief Queue ERN
         */
        std::string queueErn;

        /**
         * @brief Key
         */
        std::string key;

        /**
         * @brief Value
         */
        std::string value;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static SetQueueTagRequest fromJson(const std::string &json) {
            return boost::json::value_to<SetQueueTagRequest>(Core::ParseJsonString(json));
        }

    private:

        friend SetQueueTagRequest tag_invoke(boost::json::value_to_tag<SetQueueTagRequest>, boost::json::value const &v) {
            SetQueueTagRequest r;
            r.queueErn = Core::GetStringValue(v, "ern");
            r.key = Core::GetStringValue(v, "key");
            r.value = Core::GetStringValue(v, "value");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, SetQueueTagRequest const &obj) {
            jv = {
                    {"ern", obj.queueErn},
                    {"key", obj.key},
                    {"value", obj.value},
            };
        }
    };
}