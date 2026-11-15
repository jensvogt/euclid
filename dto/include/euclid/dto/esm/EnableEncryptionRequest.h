// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/2/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::ESM {

    using Json::tag_invoke;

    /**
     * @brief Names the bucket whose objects should be encrypted at rest, and optionally the key.
     *
     * @par
     * The key is an EKM key, named either by its ERN or by its key ID, and it is optional: a
     * request that names none has one created for the bucket. Naming one is for the case where the
     * key already exists and its lifecycle - rotation, revocation, scheduled deletion - is already
     * being managed somewhere.
     */
    struct EnableEncryptionRequest {

        /**
         * @brief ERN of the bucket to encrypt
         */
        std::string bucketErn;

        /**
         * @brief ERN or ID of the EKM key to encrypt under; empty to have one created
         */
        std::string keyId;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]]
        std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]]
        static EnableEncryptionRequest fromJson(const std::string &json) {
            return boost::json::value_to<EnableEncryptionRequest>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(EnableEncryptionRequest, (), (bucketErn, keyId))

}// namespace Euclid::Dto::ESM
