//
// Created by vogje01 on 8/31/26.
//

#pragma once

// C++ includes
#include <map>

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/com/Variant.h>

namespace Euclid::Dto::ESM {

    /**
     * @brief Replaces the user-defined attributes of one object.
     *
     * @par
     * The map sent here becomes the object's attributes: whatever it does not name is removed.
     * Adding or dropping a single attribute therefore means sending the whole map, which is what
     * makes this one action rather than the add/set/delete trio bucket tags use - an object's
     * attributes are written as a set, by whoever produced the object.
     */
    struct SetObjectAttributesRequest {

        /**
         * @brief ERN of the object
         */
        std::string ern;

        /**
         * @brief Attributes to store, replacing any the object already has
         */
        std::map<std::string, COM::Variant> attributes;

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
        static SetObjectAttributesRequest fromJson(const std::string &json) {
            return boost::json::value_to<SetObjectAttributesRequest>(Core::ParseJsonString(json));
        }

    private:

        friend SetObjectAttributesRequest tag_invoke(boost::json::value_to_tag<SetObjectAttributesRequest>, boost::json::value const &v) {
            SetObjectAttributesRequest r;
            r.ern = Core::GetStringValue(v, "ern");
            r.attributes = Core::GetMapFromObject<std::string, COM::Variant>(v, "attributes");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, SetObjectAttributesRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"attributes", boost::json::value_from(obj.attributes)},
            };
        }
    };

}// namespace Euclid::Dto::ESM
