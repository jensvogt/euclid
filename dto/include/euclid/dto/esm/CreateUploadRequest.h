// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/23/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::ESM {

    using Json::tag_invoke;

    struct CreateUploadRequest {

        /**
         * @brief ERN of the bucket the upload will be stored in
         */
        std::string bucketErn;

        /**
         * @brief Key (path) of the object being uploaded within the bucket
         */
        std::string key;

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static CreateUploadRequest fromJson(const std::string &json) {
            return boost::json::value_to<CreateUploadRequest>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(CreateUploadRequest, (), (bucketErn, key))
}// namespace Euclid::Dto::ESM
