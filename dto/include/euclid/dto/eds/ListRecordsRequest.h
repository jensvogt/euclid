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
     * @brief Lists a zone's record sets.
     */
    struct ListRecordsRequest : BaseDto {

        /**
         * @brief The zone's apex.
         */
        std::string zone;

        /**
         * @brief Only sets at this owner name, as for put-record; empty means all.
         */
        std::string name;

        /**
         * @brief Only sets of this type; empty means all.
         */
        std::string type;

        /**
         * @brief Most to return; 0 means no limit.
         */
        long pageSize{0};

        /**
         * @brief Zero-based page, applied when pageSize is set.
         */
        long pageIndex{0};

        /**
         * @brief Serializes this to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(ListRecordsRequest, (BaseDto), (zone, name, type, pageSize, pageIndex))
}
