// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/19/26.
//

#pragma once

// C++ includes
#include <string>

// C++ includes
#include <array>
#include <string_view>

// Euclid includes
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/DescribedJson.h>
#include <euclid/dto/ekm/model/Key.h>

namespace Euclid::Dto::EKM {

    using Json::tag_invoke;

    /**
     * @brief One key, as list-keys describes each of its own.
     *
     * @par
     * The same Key model rather than a shape of its own, so that what a listing shows and what this
     * shows cannot drift apart. As there, it is the key's description and never its material - that
     * never leaves the module, which is the point of a key management service.
     */
    struct GetKeyResponse : BaseDto {

        /**
         * @brief The key.
         */
        Key key;

        /**
         * @brief The members that must be in the JSON: missing or null, reading throws.
         */
        static constexpr std::array<std::string_view, 1> kJsonRequired{"key"};

        /**
         * @brief Serializes this response to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(GetKeyResponse, (BaseDto), (key))
}// namespace Euclid::Dto::EKM
