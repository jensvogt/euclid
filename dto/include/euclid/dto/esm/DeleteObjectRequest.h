//
// Created by vogje01 on 8/23/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ESM {

    struct DeleteObjectRequest {

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
        static DeleteObjectRequest fromJson(const std::string &json) {
            return boost::json::value_to<DeleteObjectRequest>(Core::ParseJsonString(json));
        }

    private:

        friend DeleteObjectRequest tag_invoke(boost::json::value_to_tag<DeleteObjectRequest>, boost::json::value const &v) {
            DeleteObjectRequest r;
            r.ern = Core::GetStringValue(v, "ern");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, DeleteObjectRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
            };
        }
    };
}// namespace Euclid::Dto::ESM