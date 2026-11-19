// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::EKM {

    using Json::tag_invoke;

    struct CreateKeyRequest {

        /**
         * @brief Name of the algorithm
         */
        std::string algorithm;

        /**
         * @brief Key length
         */
        long length = 128;

        /**
         * @brief What the key is for, kept with it for whoever finds it later. Optional.
         */
        std::string description;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static CreateKeyRequest fromJson(const std::string &json) {
            return boost::json::value_to<CreateKeyRequest>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(CreateKeyRequest, (), (algorithm, length, description))
}
