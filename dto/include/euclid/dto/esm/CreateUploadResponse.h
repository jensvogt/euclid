// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/23/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::ESM {

    using Json::tag_invoke;

    struct CreateUploadResponse : BaseDto {

        /**
         * @brief Upload ID (UUID) identifying the multipart upload
         */
        std::string uploadId;

        /**
         * @brief ERN of the bucket the upload will be stored in
         */
        std::string bucketErn;

        /**
         * @brief Key (path) of the object being uploaded within the bucket
         */
        std::string key;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(CreateUploadResponse, (BaseDto), (uploadId, bucketErn, key))
}// namespace Euclid::Dto::ESM
