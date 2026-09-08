//
// Created by vogje01 on 9/8/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ESS {

    struct DeleteSecretResponse {

        /**
         * @brief Euclid resource name of the secret that was deleted
         */
        std::string ern;

        /**
         * @brief Secret name
         */
        std::string name;

        /**
         * @brief Serializes this response to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this response from a JSON string
         */
        [[nodiscard]] static DeleteSecretResponse fromJson(const std::string &json) {
            return boost::json::value_to<DeleteSecretResponse>(Core::ParseJsonString(json));
        }

    private:

        friend DeleteSecretResponse tag_invoke(boost::json::value_to_tag<DeleteSecretResponse>, boost::json::value const &v) {
            DeleteSecretResponse r;
            r.ern = Core::GetStringValue(v, "ern");
            r.name = Core::GetStringValue(v, "name");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, DeleteSecretResponse const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"name", obj.name},
            };
        }
    };

}// namespace Euclid::Dto::ESS
