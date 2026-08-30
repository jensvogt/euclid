//
// Created by vogje01 on 8/30/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EKM {

    struct RevokeKeyRequest {

        /**
         * @brief ERN of the key to revoke
         */
        std::string ern;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static RevokeKeyRequest fromJson(const std::string &json) {
            return boost::json::value_to<RevokeKeyRequest>(Core::ParseJsonString(json));
        }

    private:

        friend RevokeKeyRequest tag_invoke(boost::json::value_to_tag<RevokeKeyRequest>, boost::json::value const &v) {
            RevokeKeyRequest r;
            r.ern = Core::GetStringValue(v, "ern");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, RevokeKeyRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
            };
        }
    };
}