//
// Created by vogje01 on 8/30/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EKM {

    struct DeleteKeyRequest {

        /**
         * @brief ID of the key to delete (as returned by create-key)
         */
        std::string keyId;

        /**
         * @brief Number of days to wait before the key is permanently deleted
         */
        long pendingWindowInDays = 7;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static DeleteKeyRequest fromJson(const std::string &json) {
            return boost::json::value_to<DeleteKeyRequest>(Core::ParseJsonString(json));
        }

    private:

        friend DeleteKeyRequest tag_invoke(boost::json::value_to_tag<DeleteKeyRequest>, boost::json::value const &v) {
            DeleteKeyRequest r;
            r.keyId = Core::GetStringValue(v, "keyId");
            r.pendingWindowInDays = Core::GetLongValue(v, "pendingWindowInDays", 7);
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, DeleteKeyRequest const &obj) {
            jv = {
                    {"keyId", obj.keyId},
                    {"pendingWindowInDays", obj.pendingWindowInDays},
            };
        }
    };
}
