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

    struct UploadPartResponse : BaseDto {

        /**
         * @brief Upload ID (UUID) this part belongs to
         */
        std::string uploadId;

        /**
         * @brief 1-based part number
         */
        long partNumber = 0;

        /**
         * @brief Size of the stored part, in bytes
         */
        long size = 0;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(UploadPartResponse, (BaseDto), (uploadId, partNumber, size))
}// namespace Euclid::Dto::ESM
