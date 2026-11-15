// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/24/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::ESM {

    using Json::tag_invoke;

    struct CreateDownloadRequest {

        /**
         * @brief ERN of the bucket the object is stored in
         */
        std::string bucketErn;

        /**
         * @brief Key (path) of the object being downloaded within the bucket
         */
        std::string key;

        /**
         * @brief A non-current version of the object to download instead; empty for the current one.
         */
        std::string versionId;

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static CreateDownloadRequest fromJson(const std::string &json) {
            return boost::json::value_to<CreateDownloadRequest>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(CreateDownloadRequest, (), (bucketErn, key, versionId))
}// namespace Euclid::Dto::ESM
