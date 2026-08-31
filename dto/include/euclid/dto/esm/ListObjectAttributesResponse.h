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
     * @brief Every attribute an object carries.
     */
    struct ListObjectAttributesResponse {

        /**
         * @brief ERN of the object
         */
        std::string ern;

        /**
         * @brief Attributes, by name
         */
        std::map<std::string, COM::Variant> attributes;

        /**
         * @brief Number of attributes
         */
        long total{};

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
        static ListObjectAttributesResponse fromJson(const std::string &json) {
            return boost::json::value_to<ListObjectAttributesResponse>(Core::ParseJsonString(json));
        }

    private:

        friend ListObjectAttributesResponse tag_invoke(boost::json::value_to_tag<ListObjectAttributesResponse>, boost::json::value const &v) {
            ListObjectAttributesResponse r;
            r.ern = Core::GetStringValue(v, "ern");
            r.attributes = Core::GetMapFromObject<std::string, COM::Variant>(v, "attributes");
            r.total = Core::GetLongValue(v, "total");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ListObjectAttributesResponse const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"attributes", boost::json::value_from(obj.attributes)},
                    {"total", obj.total},
            };
        }
    };

}// namespace Euclid::Dto::ESM
