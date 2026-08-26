//
// Created by vogje01 on 8/18/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ENS {

    struct GetMessageAttributeRequest {

        /**
         * @brief Message ID
         */
        std::string messageId{};

        /**
         * @brief Key of the attribute to look up
         */
        std::string key{};

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
        static GetMessageAttributeRequest fromJson(const std::string &json) {
            return boost::json::value_to<GetMessageAttributeRequest>(Core::ParseJsonString(json));
        }

    private:

        friend GetMessageAttributeRequest tag_invoke(boost::json::value_to_tag<GetMessageAttributeRequest>, boost::json::value const &v) {
            GetMessageAttributeRequest r;
            r.messageId = Core::GetStringValue(v, "messageId");
            r.key = Core::GetStringValue(v, "key");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, GetMessageAttributeRequest const &obj) {
            jv = {
                    {"messageId", obj.messageId},
                    {"key", obj.key},
            };
        }
    };

}