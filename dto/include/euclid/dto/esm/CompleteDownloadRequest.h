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

    struct CompleteDownloadRequest {

        /**
         * @brief Download ID (UUID) returned by create-download
         */
        std::string downloadId;

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static CompleteDownloadRequest fromJson(const std::string &json) {
            return boost::json::value_to<CompleteDownloadRequest>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(CompleteDownloadRequest, (), (downloadId))
}// namespace Euclid::Dto::ESM
