//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EKM {

    struct CreateKeyResponse {

        /**
         * @brief Euclid resource name
         */
        std::string ern;

        /**
         * @brief Key name
         */
        std::string name;

        /**
         * @brief Name of the algorithm
         */
        std::string algorithm;

        /**
         * @brief Key length
         */
        long length = 128;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static CreateKeyResponse fromJson(const std::string &json) {
            return boost::json::value_to<CreateKeyResponse>(Core::ParseJsonString(json));
        }

    private:

        friend CreateKeyResponse tag_invoke(boost::json::value_to_tag<CreateKeyResponse>, boost::json::value const &v) {
            CreateKeyResponse r;
            r.ern = Core::GetStringValue(v, "ern");
            r.name = Core::GetStringValue(v, "name");
            r.algorithm = Core::GetStringValue(v, "algorithm");
            r.length = Core::GetLongValue(v, "length");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, CreateKeyResponse const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"name", obj.name},
                    {"algorithm", obj.algorithm},
                    {"length", obj.length},
            };
        }
    };
}