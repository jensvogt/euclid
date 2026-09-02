//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EKM {

    struct CreateKeyRequest {

        /**
         * @brief Name of the algorithm
         */
        std::string algorithm;

        /**
         * @brief Key length
         */
        long length = 128;

        /**
         * @brief What the key is for, kept with it for whoever finds it later. Optional.
         */
        std::string description;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static CreateKeyRequest fromJson(const std::string &json) {
            return boost::json::value_to<CreateKeyRequest>(Core::ParseJsonString(json));
        }

    private:

        friend CreateKeyRequest tag_invoke(boost::json::value_to_tag<CreateKeyRequest>, boost::json::value const &v) {
            CreateKeyRequest r;
            r.algorithm = Core::GetStringValue(v, "algorithm");
            r.length = Core::GetLongValue(v, "length");
            r.description = Core::GetStringValue(v, "description");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, CreateKeyRequest const &obj) {
            jv = {
                    {"algorithm", obj.algorithm},
                    {"length", obj.length},
                    {"description", obj.description},
            };
        }
    };
}