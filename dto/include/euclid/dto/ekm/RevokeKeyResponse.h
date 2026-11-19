// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/30/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::EKM {

    using Json::tag_invoke;

    struct RevokeKeyResponse : BaseDto {

        /**
         * @brief Euclid resource name
         */
        std::string ern;

        /**
         * @brief Key name
         */
        std::string name;

        /**
         * @brief Lifecycle status; always "REVOKED" after a successful revoke-key
         */
        std::string status = "REVOKED";

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static RevokeKeyResponse fromJson(const std::string &json) {
            return boost::json::value_to<RevokeKeyResponse>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(RevokeKeyResponse, (BaseDto), (ern, name, status))
}
