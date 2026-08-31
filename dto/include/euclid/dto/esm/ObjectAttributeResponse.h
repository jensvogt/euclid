//
// Created by vogje01 on 8/31/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/com/Variant.h>

namespace Euclid::Dto::ESM {

    /**
     * @brief One object attribute, as it now stands in the store.
     */
    struct ObjectAttributeResponse {

        /**
         * @brief ERN of the object
         */
        std::string ern;

        /**
         * @brief Attribute name
         */
        std::string name;

        /**
         * @brief Attribute value, typed
         */
        COM::Variant value;

        /**
         * @brief Serializes this response to a JSON string
         */
        [[nodiscard]]
        std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this response from a JSON string
         */
        [[nodiscard]]
        static ObjectAttributeResponse fromJson(const std::string &json) {
            return boost::json::value_to<ObjectAttributeResponse>(Core::ParseJsonString(json));
        }

    private:

        friend ObjectAttributeResponse tag_invoke(boost::json::value_to_tag<ObjectAttributeResponse>, boost::json::value const &v) {
            ObjectAttributeResponse r;
            r.ern = Core::GetStringValue(v, "ern");
            r.name = Core::GetStringValue(v, "name");
            if (Core::AttributeExists(v, "value")) r.value = boost::json::value_to<COM::Variant>(v.at("value"));
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ObjectAttributeResponse const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"name", obj.name},
                    {"value", boost::json::value_from(obj.value)},
            };
        }
    };

}// namespace Euclid::Dto::ESM
