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

    struct DeleteKeyResponse : BaseDto {

        /**
         * @brief Euclid resource name
         */
        std::string ern;

        /**
         * @brief Key name
         */
        std::string name;

        /**
         * @brief Point in time at which the key will be permanently deleted, ISO8601
         */
        std::string deletionDate;

        /**
         * @brief Lifecycle status; always "PENDING_DELETION" after a successful delete-key
         */
        std::string status = "PENDING_DELETION";

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static DeleteKeyResponse fromJson(const std::string &json) {
            return boost::json::value_to<DeleteKeyResponse>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(DeleteKeyResponse, (BaseDto), (ern, name, deletionDate, status))
}
