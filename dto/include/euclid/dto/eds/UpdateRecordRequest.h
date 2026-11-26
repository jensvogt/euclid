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
     * @brief Changes part of a record set - its TTL, values removed, values added - without resending the rest.
     */
    struct UpdateRecordRequest : BaseDto {

        /**
         * @brief The zone's apex.
         */
        std::string zone;

        /**
         * @brief Owner name, as for put-record.
         */
        std::string name;

        /**
         * @brief Type mnemonic.
         */
        std::string type;

        /**
         * @brief New TTL in seconds; 0 takes the zone's. Only applied when the request carries it.
         */
        long ttl{0};

        /**
         * @brief Values to add, in presentation form. One already in the set is not added twice.
         */
        std::vector<std::string> addValues;

        /**
         * @brief Values to remove. Each has to be in the set; they are removed before any is added.
         */
        std::vector<std::string> removeValues;

        /**
         * @brief Serializes this to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(UpdateRecordRequest, (BaseDto), (zone, name, type, ttl, addValues, removeValues))
}
