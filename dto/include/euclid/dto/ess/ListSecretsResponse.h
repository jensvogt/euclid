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
#include <vector>

// Euclid includes
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/DescribedJson.h>
#include <euclid/dto/ess/model/Secret.h>

namespace Euclid::Dto::ESS {

    using Json::tag_invoke;

    /**
     * @brief The secrets an account holds, without any of their values.
     */
    struct ListSecretsResponse : BaseDto {

        /**
         * @brief Secrets list
         */
        std::vector<Secret> secrets;

        /**
         * @brief Total number of secrets matching the request, ignoring paging
         */
        long total{};

        /**
         * @brief The members that must be in the JSON: missing or null, reading throws.
         */
        static constexpr std::array<std::string_view, 1> kJsonRequired{"secrets"};

        /**
         * @brief Serializes this response to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(ListSecretsResponse, (BaseDto), (secrets, total))

}// namespace Euclid::Dto::ESS
