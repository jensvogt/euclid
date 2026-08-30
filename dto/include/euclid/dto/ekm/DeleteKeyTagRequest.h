#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EKM {

    struct DeleteKeyTagRequest {

        /**
         * @brief Key ERN
         */
        std::string ern;

        /**
         * @brief Key
         */
        std::string key;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static DeleteKeyTagRequest fromJson(const std::string &json) {
            return boost::json::value_to<DeleteKeyTagRequest>(Core::ParseJsonString(json));
        }

    private:

        friend DeleteKeyTagRequest tag_invoke(boost::json::value_to_tag<DeleteKeyTagRequest>, boost::json::value const &v) {
            DeleteKeyTagRequest r;
            r.ern = Core::GetStringValue(v, "ern");
            r.key = Core::GetStringValue(v, "key");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, DeleteKeyTagRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"key", obj.key},
            };
        }
    };
}
