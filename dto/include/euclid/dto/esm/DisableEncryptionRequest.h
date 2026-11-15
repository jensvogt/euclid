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
     * @brief Names the bucket whose objects should stop being encrypted at rest.
     *
     * @par
     * No key is named, because none is needed and none is touched: this says what happens to the
     * next object written to the bucket, and nothing about the objects already in it or the key
     * they are under.
     */
    struct DisableEncryptionRequest {

        /**
         * @brief ERN of the bucket to stop encrypting
         */
        std::string bucketErn;

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
        static DisableEncryptionRequest fromJson(const std::string &json) {
            return boost::json::value_to<DisableEncryptionRequest>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(DisableEncryptionRequest, (), (bucketErn))

}// namespace Euclid::Dto::ESM
