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
     * @brief The bucket's encryption as it now stands, and how many objects it does not cover.
     */
    struct EnableEncryptionResponse : BaseDto {

        /**
         * @brief ERN of the bucket
         */
        std::string ern;

        /**
         * @brief Name of the bucket
         */
        std::string name;

        /**
         * @brief ERN of the EKM key objects written from now on are encrypted under
         */
        std::string keyErn;

        /**
         * @brief ID of that key, as "ekm list-keys" and "ekm delete-key" refer to it
         */
        std::string keyId;

        /**
         * @brief Algorithm and length of the key, e.g. "AES-256"
         */
        std::string algorithm;

        /**
         * @brief Whether the key was created for this bucket by this call, rather than named by it
         */
        bool keyCreated = false;

        /**
         * @brief Number of objects the bucket already holds.
         *
         * @par
         * They are left exactly as they were stored - enabling encryption says what happens to the
         * next object written, and rewriting what is already there is a copy, not a setting. Worth
         * reporting because it is the number an operator has to do something about if the point of
         * enabling encryption was that nothing in the bucket should be readable off the disk.
         */
        long existingObjects = 0;

        /**
         * @brief Serializes this response to a JSON string
         */
        [[nodiscard]]
        std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(EnableEncryptionResponse, (BaseDto), (ern, name, keyErn, keyId, algorithm, keyCreated, existingObjects))

}// namespace Euclid::Dto::ESM
