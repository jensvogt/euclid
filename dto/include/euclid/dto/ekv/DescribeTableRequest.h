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

namespace Euclid::Dto::EKV {

    /**
     * @brief Asks what a table looks like.
     */
    struct DescribeTableRequest : BaseDto {

        /**
         * @brief Table to describe.
         */
        std::string name;

        /**
         * @brief Serializes this to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend DescribeTableRequest tag_invoke(boost::json::value_to_tag<DescribeTableRequest>, boost::json::value const &v) {
            DescribeTableRequest r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.name = Core::GetStringValue(v, "name");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, DescribeTableRequest const &obj) {
            jv = {
                    {"metadata", boost::json::value_from(static_cast<const BaseDto &>(obj))},
                    {"name", obj.name},
            };
        }
    };
}
