// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/8/26.
//

#pragma once

// C++ includes
#include <array>
#include <string_view>

// Euclid includes
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/DescribedJson.h>
#include <euclid/dto/ess/model/Secret.h>

namespace Euclid::Dto::ESS {

    using Json::tag_invoke;

    /**
     * @brief One secret with its value, as answered by get-secret and nothing else.
     */
    struct GetSecretResponse : BaseDto {

        /**
         * @brief The secret's metadata
         */
        Secret secret;

        /**
         * @brief The value, decrypted. The only place in euclid where one of these is written out.
         */
        std::string value;

        /**
         * @brief The members that must be in the JSON: missing or null, reading throws.
         */
        static constexpr std::array<std::string_view, 1> kJsonRequired{"secret"};

        /**
         * @brief Serializes this response to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(GetSecretResponse, (BaseDto), (secret, value))

}// namespace Euclid::Dto::ESS
