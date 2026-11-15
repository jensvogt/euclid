// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/2/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::ESM {

    using Json::tag_invoke;

    /**
     * @brief The bucket with encryption switched off, and what is still encrypted inside it.
     */
    struct DisableEncryptionResponse : BaseDto {

        /**
         * @brief ERN of the bucket
         */
        std::string ern;

        /**
         * @brief Name of the bucket
         */
        std::string name;

        /**
         * @brief ERN of the key the bucket was encrypting under, empty if it was not encrypting.
         *
         * @par
         * The key itself is left exactly as it was - not revoked, not scheduled for deletion, not
         * even untagged. It has to be: every object written while it was in force is still under
         * it, and deleting it is what would make those unreadable.
         */
        std::string previousKeyErn;

        /**
         * @brief ID of that key, as "ekm list-keys" refers to it
         */
        std::string previousKeyId;

        /**
         * @brief Number of objects in the bucket that are still stored encrypted.
         *
         * @par
         * Nothing is decrypted by switching encryption off, so this is what the bucket still holds
         * under one key or another - and while it is above zero, the keys those objects name have
         * to stay in EKM.
         */
        long encryptedObjects = 0;

        /**
         * @brief Serializes this response to a JSON string
         */
        [[nodiscard]]
        std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(DisableEncryptionResponse, (BaseDto), (ern, name, previousKeyErn, previousKeyId, encryptedObjects))

}// namespace Euclid::Dto::ESM
