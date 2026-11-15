// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/18/26.
//

#pragma once

// C++ includes
#include <array>
#include <string_view>

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/com/Variant.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::EQS {

    using Json::tag_invoke;

    struct GetMessageAttributeResponse : BaseDto {

        /**
         * @brief Message ID
         */
        std::string messageId{};

        /**
         * @brief Name of the attribute
         */
        std::string name{};

        /**
         * @brief Value of the attribute
         */
        COM::Variant value{};

        /**
         * @brief The members that must be in the JSON: missing or null, reading throws.
         */
        static constexpr std::array<std::string_view, 1> kJsonRequired{"value"};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(GetMessageAttributeResponse, (BaseDto), (messageId, name, value))

}