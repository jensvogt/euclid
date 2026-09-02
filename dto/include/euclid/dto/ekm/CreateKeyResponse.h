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
         * @brief What the key is for, as it was given; empty if none was
         */
        std::string description;

        /**
         * @brief Name of the algorithm
         */
        std::string algorithm;

        /**
         * @brief Key length
         */
        long length = 128;

        /**
         * @brief Lifecycle status; always "AVAILABLE" for a freshly created key
         */
        std::string status = "AVAILABLE";

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
            r.description = Core::GetStringValue(v, "description");
            r.algorithm = Core::GetStringValue(v, "algorithm");
            r.length = Core::GetLongValue(v, "length");
            r.status = Core::GetStringValue(v, "status");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, CreateKeyResponse const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"name", obj.name},
                    {"description", obj.description},
                    {"algorithm", obj.algorithm},
                    {"length", obj.length},
                    {"status", obj.status},
            };
        }
    };
}