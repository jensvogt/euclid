// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/10/26.
//

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

namespace Euclid::Dto::EKV {

    using Json::tag_invoke;

    /**
     * @brief Writes an item, replacing whatever was stored under its key.
     */
    struct PutItemRequest : BaseDto {

        /**
         * @brief Table to write to.
         */
        std::string table;

        /**
         * @brief The item, as an object. It has to carry the table's key attributes.
         */
        boost::json::value item;

        /**
         * @brief Serializes this to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(PutItemRequest, (BaseDto), (table, item))
}
