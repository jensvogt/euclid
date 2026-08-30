//
// Created by vogje01 on 8/30/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EKM {

    struct DeleteKeyResponse {

        /**
         * @brief Euclid resource name
         */
        std::string ern;

        /**
         * @brief Key name
         */
        std::string name;

        /**
         * @brief Point in time at which the key will be permanently deleted, ISO8601
         */
        std::string deletionDate;

        /**
         * @brief Lifecycle status; always "PENDING_DELETION" after a successful delete-key
         */
        std::string status = "PENDING_DELETION";

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static DeleteKeyResponse fromJson(const std::string &json) {
            return boost::json::value_to<DeleteKeyResponse>(Core::ParseJsonString(json));
        }

    private:

        friend DeleteKeyResponse tag_invoke(boost::json::value_to_tag<DeleteKeyResponse>, boost::json::value const &v) {
            DeleteKeyResponse r;
            r.ern = Core::GetStringValue(v, "ern");
            r.name = Core::GetStringValue(v, "name");
            r.deletionDate = Core::GetStringValue(v, "deletionDate");
            r.status = Core::GetStringValue(v, "status");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, DeleteKeyResponse const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"name", obj.name},
                    {"deletionDate", obj.deletionDate},
                    {"status", obj.status},
            };
        }
    };
}
