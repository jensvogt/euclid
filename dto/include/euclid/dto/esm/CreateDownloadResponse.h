// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/24/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::ESM {

    using Json::tag_invoke;

    struct CreateDownloadResponse : BaseDto {

        /**
         * @brief Download ID (UUID) identifying the multipart download
         */
        std::string downloadId;

        /**
         * @brief ERN of the bucket the object is stored in
         */
        std::string bucketErn;

        /**
         * @brief Key (path) of the object being downloaded within the bucket
         */
        std::string key;

        /**
         * @brief Euclid resource name of the object being downloaded
         */
        std::string ern;

        /**
         * @brief Object size in bytes, needed by the caller to plan how many parts to request
         */
        long size = 0;

        /**
         * @brief MIME content type of the object
         */
        std::string contentType;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(CreateDownloadResponse, (BaseDto), (downloadId, bucketErn, key, ern, size, contentType))
}// namespace Euclid::Dto::ESM
