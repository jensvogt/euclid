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
     * @brief Writes a record set, replacing the one of the same name and type.
     */
    struct PutRecordRequest : BaseDto {

        /**
         * @brief The zone's apex.
         */
        std::string zone;

        /**
         * @brief Owner name: relative to the zone ("www"), fully qualified ("www.example.com"), "@" for the apex, or a wildcard ("*.dev").
         */
        std::string name;

        /**
         * @brief Type mnemonic: A, AAAA, CNAME, MX, NS, PTR, SRV or TXT.
         */
        std::string type;

        /**
         * @brief TTL in seconds; 0 takes the zone's.
         */
        long ttl{0};

        /**
         * @brief The values, in zone-file presentation form - see Dns::EncodeRdata().
         */
        std::vector<std::string> values;

        /**
         * @brief Serializes this to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(PutRecordRequest, (BaseDto), (zone, name, type, ttl, values))
}
