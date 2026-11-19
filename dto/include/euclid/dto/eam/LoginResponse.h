// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <string>

// Boost includes
#include <boost/json/conversion.hpp>

// Euclid include
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::EAM {

    using Json::tag_invoke;

    struct LoginResponse : BaseDto {

        /**
         * @brief JWT token
         */
        std::string token;

        /**
         * @brief Public identifier of the caller's SigV4 access key, e.g. "AKIA...". Reused from
         * an existing active key if the user already has one, otherwise provisioned on this login.
         */
        std::string accessKeyId;

        /**
         * @brief Secret used to sign requests, paired with accessKeyId. May be a value already
         * returned on a prior login (not necessarily freshly generated), since login reuses an
         * existing active key rather than minting a new one each time.
         */
        std::string secretAccessKey;

        /**
         * @brief Creation timestamp of the access key, ISO8601 (not the login time).
         */
        std::string createdAt;

        /**
         * @brief Whether the logged-in user has administrator privileges. The CLI stores this
         * alongside the token so admin-only commands (e.g. "emm") can be rejected locally
         * without a round trip, though the server enforces this independently regardless.
         */
        bool isAdmin{false};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(LoginResponse, (BaseDto), (token, accessKeyId, secretAccessKey, createdAt, isAdmin))

}