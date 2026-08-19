//
// Created by vogje01 on 8/18/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::SQS {

    struct GetMessageAttributeRequest {

        /**
         * @brief Message ID
         */
        std::string messageId{};

        /**
         * @brief Name of the attribute to look up
         */
        std::string name{};

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
            r.name = Core::GetStringValue(v, "name");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, GetMessageAttributeRequest const &obj) {
            jv = {
                    {"messageId", obj.messageId},
                    {"name", obj.name},
            };
        }
    };

}
