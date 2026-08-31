//
// Created by vogje01 on 8/31/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ESM {

    /**
     * @brief Names the object attribute to remove.
     */
    struct DeleteObjectAttributeRequest {

        /**
         * @brief ERN of the object
         */
        std::string ern;

        /**
         * @brief Attribute name
         */
        std::string name;

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
        static DeleteObjectAttributeRequest fromJson(const std::string &json) {
            return boost::json::value_to<DeleteObjectAttributeRequest>(Core::ParseJsonString(json));
        }

    private:

        friend DeleteObjectAttributeRequest tag_invoke(boost::json::value_to_tag<DeleteObjectAttributeRequest>, boost::json::value const &v) {
            DeleteObjectAttributeRequest r;
            r.ern = Core::GetStringValue(v, "ern");
            r.name = Core::GetStringValue(v, "name");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, DeleteObjectAttributeRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"name", obj.name},
            };
        }
    };

}// namespace Euclid::Dto::ESM
