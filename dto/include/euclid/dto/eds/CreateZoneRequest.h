// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <string>
#include <vector>

// Boost includes
#include <boost/json.hpp>

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::EDS {

    using Json::tag_invoke;

    /**
     * @brief Creates a zone.
     */
    struct CreateZoneRequest : BaseDto {

        /**
         * @brief The apex, e.g. "example.com". A trailing dot is accepted and dropped.
         */
        std::string name;

        /**
         * @brief TTL in seconds of records that name none, and of negative answers; 0 means 300.
         */
        long ttl{0};

        /**
         * @brief Free text.
         */
        std::string description;

        /**
         * @brief Serializes this to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(CreateZoneRequest, (BaseDto), (name, ttl, description))
}
