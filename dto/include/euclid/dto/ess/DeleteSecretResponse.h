// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/8/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::ESS {

    using Json::tag_invoke;

    struct DeleteSecretResponse : BaseDto {

        /**
         * @brief Euclid resource name of the secret that was deleted
         */
        std::string ern;

        /**
         * @brief Secret name
         */
        std::string name;

        /**
         * @brief Serializes this response to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this response from a JSON string
         */
        [[nodiscard]] static DeleteSecretResponse fromJson(const std::string &json) {
            return boost::json::value_to<DeleteSecretResponse>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(DeleteSecretResponse, (BaseDto), (ern, name))

}// namespace Euclid::Dto::ESS
