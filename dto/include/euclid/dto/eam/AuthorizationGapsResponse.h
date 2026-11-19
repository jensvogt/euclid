// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/12/26.
//

#pragma once

// C++ includes
#include <array>
#include <string>
#include <string_view>
#include <vector>

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/eam/model/AuthorizationGap.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::EAM {

    using Json::tag_invoke;

    /**
     * @brief What shadow mode has recorded, most-seen first.
     */
    struct AuthorizationGapsResponse : BaseDto {

        /**
         * @brief The gaps, most-seen first - the one refused ten thousand times is the grant to
         * write first.
         */
        std::vector<AuthorizationGap> gaps;

        /**
         * @brief How many distinct gaps have been recorded in total.
         */
        long total{};

        /**
         * @brief What euclid.eam.authorization currently says, so a caller can tell an empty list
         * that means "nothing is missing" from one that means "nothing has been watching".
         */
        std::string mode;

        /**
         * @brief The members that must be in the JSON: missing or null, reading throws.
         */
        static constexpr std::array<std::string_view, 1> kJsonRequired{"gaps"};

        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(AuthorizationGapsResponse, (BaseDto), (gaps, total, mode))

}
