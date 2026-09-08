//
// Created by vogje01 on 9/8/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ESS {

    /**
     * @brief Changes a stored secret: its value, its description, or the key it is encrypted
     * under. Only what is named changes, so rotating a value does not require restating anything
     * else about the secret.
     */
    struct UpdateSecretRequest {

        /**
         * @brief Name of the secret to change
         */
        std::string name;

        /**
         * @brief The new value, or absent to leave it as it is. Setting it is a rotation: the
         * version moves on and the previous value is gone.
         */
        std::string value;

        /**
         * @brief Whether "value" was given at all, which is what tells a rotation from a change of
         * description. Needed because an empty string is a value somebody may legitimately store.
         */
        bool hasValue{false};

        /**
         * @brief The new description, or absent to leave it as it is
         */
        std::string description;

        /**
         * @brief Whether "description" was given at all - an empty one clears it, so absent and
         * empty cannot mean the same thing.
         */
        bool hasDescription{false};

        /**
         * @brief ERN of another EKM key to encrypt the secret under from now on, or absent to keep
         * the current one. Naming one re-encrypts the value, which is how a secret is moved off a
         * key that is being retired.
         */
        std::string keyErn;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend UpdateSecretRequest tag_invoke(boost::json::value_to_tag<UpdateSecretRequest>, boost::json::value const &v) {
            UpdateSecretRequest r;
            r.name = Core::GetStringValue(v, "name");
            r.hasValue = Core::AttributeExists(v, "value");
            r.value = Core::GetStringValue(v, "value");
            r.hasDescription = Core::AttributeExists(v, "description");
            r.description = Core::GetStringValue(v, "description");
            r.keyErn = Core::GetStringValue(v, "keyErn");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, UpdateSecretRequest const &obj) {
            boost::json::object out{{"name", obj.name}};
            if (obj.hasValue) out["value"] = obj.value;
            if (obj.hasDescription) out["description"] = obj.description;
            if (!obj.keyErn.empty()) out["keyErn"] = obj.keyErn;
            jv = out;
        }
    };

}// namespace Euclid::Dto::ESS
