// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/8/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::ESS {

    using Json::tag_invoke;

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
    };

    BOOST_DESCRIBE_STRUCT(CreateSecretRequest, (), (name, description, value, keyErn))

}// namespace Euclid::Dto::ESS
