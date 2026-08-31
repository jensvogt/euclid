//
// Created by vogje01 on 8/31/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/com/Variant.h>

namespace Euclid::Dto::ESM {

    /**
     * @brief Names one attribute of one object, with the value to store.
     *
     * @par
     * Shared by add-object-attribute and set-object-attribute, which differ only in what they
     * expect to find already there: adding refuses an attribute that exists, setting refuses one
     * that does not. The two carry identical fields, so one request type serves both - the same
     * way EQS's queue tags reuse AddQueueTagRequest for add and set.
     */
    struct ObjectAttributeRequest {

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
        static ObjectAttributeRequest fromJson(const std::string &json) {
            return boost::json::value_to<ObjectAttributeRequest>(Core::ParseJsonString(json));
        }

    private:

        friend ObjectAttributeRequest tag_invoke(boost::json::value_to_tag<ObjectAttributeRequest>, boost::json::value const &v) {
            ObjectAttributeRequest r;
            r.ern = Core::GetStringValue(v, "ern");
            r.name = Core::GetStringValue(v, "name");
            if (Core::AttributeExists(v, "value")) r.value = boost::json::value_to<COM::Variant>(v.at("value"));
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ObjectAttributeRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"name", obj.name},
                    {"value", boost::json::value_from(obj.value)},
            };
        }
    };

}// namespace Euclid::Dto::ESM
