//
// Created by vogje01 on 8/31/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ESM {

    /**
     * @brief Names the object whose attributes are listed.
     */
    struct ListObjectAttributesRequest {

        /**
         * @brief ERN of the object
         */
        std::string ern;

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
        static ListObjectAttributesRequest fromJson(const std::string &json) {
            return boost::json::value_to<ListObjectAttributesRequest>(Core::ParseJsonString(json));
        }

    private:

        friend ListObjectAttributesRequest tag_invoke(boost::json::value_to_tag<ListObjectAttributesRequest>, boost::json::value const &v) {
            ListObjectAttributesRequest r;
            r.ern = Core::GetStringValue(v, "ern");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ListObjectAttributesRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
            };
        }
    };

}// namespace Euclid::Dto::ESM
