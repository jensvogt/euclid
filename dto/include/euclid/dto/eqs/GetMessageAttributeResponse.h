//
// Created by vogje01 on 8/18/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/com/Variant.h>

namespace Euclid::Dto::EQS {

    struct GetMessageAttributeResponse {

        /**
         * @brief Message ID
         */
        std::string messageId{};

        /**
         * @brief Name of the attribute
         */
        std::string name{};

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

    private:

        friend GetMessageAttributeResponse tag_invoke(boost::json::value_to_tag<GetMessageAttributeResponse>, boost::json::value const &v) {
            GetMessageAttributeResponse r;
            r.messageId = Core::GetStringValue(v, "messageId");
            r.name = Core::GetStringValue(v, "name");
            r.value = boost::json::value_to<COM::Variant>(v.at("value"));
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, GetMessageAttributeResponse const &obj) {
            jv = {
                    {"messageId", obj.messageId},
                    {"name", obj.name},
                    {"value", boost::json::value_from(obj.value)},
            };
        }
    };

}