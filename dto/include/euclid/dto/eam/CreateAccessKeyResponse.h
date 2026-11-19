// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/18/26.
//

#pragma once

// C++ includes
#include <string>

// Euclid includes
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::EAM {

    using Json::tag_invoke;

    struct CreateAccessKeyResponse : BaseDto {

        /**
         * @brief Public identifier of the newly created key, e.g. "AKIA...".
         */
        std::string accessKeyId;

        /**
         * @brief Secret used to sign requests. Shown only this once via this endpoint - not
         * retrievable again here, though "access login" will also return it if this key is later
         * reused as the caller's active key.
         */
        std::string secretAccessKey;

        /**
         * @brief Creation timestamp, ISO8601.
         */
        std::string createdAt;

        /**
         * @brief Serializes this response to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(CreateAccessKeyResponse, (BaseDto), (accessKeyId, secretAccessKey, createdAt))

}
