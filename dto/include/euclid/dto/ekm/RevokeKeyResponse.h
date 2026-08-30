//
// Created by vogje01 on 8/30/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EKM {

    struct RevokeKeyResponse {

        /**
         * @brief Euclid resource name
         */
        std::string ern;

        /**
         * @brief Key name
         */
        std::string name;

        /**
         * @brief Lifecycle status; always "REVOKED" after a successful revoke-key
         */
        std::string status = "REVOKED";

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static RevokeKeyResponse fromJson(const std::string &json) {
            return boost::json::value_to<RevokeKeyResponse>(Core::ParseJsonString(json));
        }

    private:

        friend RevokeKeyResponse tag_invoke(boost::json::value_to_tag<RevokeKeyResponse>, boost::json::value const &v) {
            RevokeKeyResponse r;
            r.ern = Core::GetStringValue(v, "ern");
            r.name = Core::GetStringValue(v, "name");
            r.status = Core::GetStringValue(v, "status");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, RevokeKeyResponse const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"name", obj.name},
                    {"status", obj.status},
            };
        }
    };
}
