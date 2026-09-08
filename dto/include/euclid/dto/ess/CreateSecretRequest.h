//
// Created by vogje01 on 9/8/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ESS {

    /**
     * @brief Stores a new secret.
     */
    struct CreateSecretRequest {

        /**
         * @brief Name the secret is stored under, and the one an application asks for. Unique
         * within the caller's account and namespace.
         */
        std::string name;

        /**
         * @brief What the secret is for. Optional, free text, and readable by anyone who may list
         * secrets - so not the place for any part of the value.
         */
        std::string description;

        /**
         * @brief The secret itself: a password, a connection string, a token, or JSON holding
         * several of those. Encrypted before it is stored and never written down anywhere else.
         */
        std::string value;

        /**
         * @brief ERN of the EKM key to encrypt it under. Empty asks the module for the namespace's
         * own secrets key, which it creates the first time something needs it - a secrets store
         * that fell back to storing values in the clear would be worse than useless.
         */
        std::string keyErn;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static CreateSecretRequest fromJson(const std::string &json) {
            return boost::json::value_to<CreateSecretRequest>(Core::ParseJsonString(json));
        }

    private:

        friend CreateSecretRequest tag_invoke(boost::json::value_to_tag<CreateSecretRequest>, boost::json::value const &v) {
            CreateSecretRequest r;
            r.name = Core::GetStringValue(v, "name");
            r.description = Core::GetStringValue(v, "description");
            r.value = Core::GetStringValue(v, "value");
            r.keyErn = Core::GetStringValue(v, "keyErn");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, CreateSecretRequest const &obj) {
            jv = {
                    {"name", obj.name},
                    {"description", obj.description},
                    {"value", obj.value},
                    {"keyErn", obj.keyErn},
            };
        }
    };

}// namespace Euclid::Dto::ESS
