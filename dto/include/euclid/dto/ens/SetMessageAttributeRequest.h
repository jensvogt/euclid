//
// Created by vogje01 on 8/18/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ENS {

    struct SetMessageAttributeRequest {

        /**
         * @brief Message ID
         */
        std::string messageId{};

        /**
         * @brief Key of the attribute to look up
         */
        std::string key{};

        /**
         * @brief Value of the attribute
         */
        COM::Variant value{};

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
        static SetMessageAttributeRequest fromJson(const std::string &json) {
            return boost::json::value_to<SetMessageAttributeRequest>(Core::ParseJsonString(json));
        }

    private:

        friend SetMessageAttributeRequest tag_invoke(boost::json::value_to_tag<SetMessageAttributeRequest>, boost::json::value const &v) {
            SetMessageAttributeRequest r;
            r.messageId = Core::GetStringValue(v, "messageId");
            r.key = Core::GetStringValue(v, "key");
            r.value = boost::json::value_to<COM::Variant>(v, "value");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, SetMessageAttributeRequest const &obj) {
            jv = {
                    {"messageId", obj.messageId},
                    {"key", obj.key},
                    {"value", boost::json::value_from(obj.value)},
            };
        }
    };

}