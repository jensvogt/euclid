// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/12/26.
//

#pragma once

// C++ includes
#include <string>
#include <vector>

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/eam/model/Grant.h>

namespace Euclid::Dto::EAM {

    /**
     * @brief The grants matching a principal or a role.
     */
    struct ListGrantsResponse : BaseDto {

        /**
         * @brief The grants.
         */
        std::vector<Grant> grants;

        /**
         * @brief How many there are.
         */
        long total{};

        /**
         * @brief Serializes this to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend ListGrantsResponse tag_invoke(boost::json::value_to_tag<ListGrantsResponse>, boost::json::value const &v) {
            ListGrantsResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            for (const auto &grant: v.at("grants").as_array()) r.grants.push_back(boost::json::value_to<Grant>(grant));
            r.total = Core::GetLongValue(v, "total");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ListGrantsResponse const &obj) {
            jv = {
                    {"metadata", boost::json::value_from(static_cast<const BaseDto &>(obj))},
                    {"grants", boost::json::value_from(obj.grants)},
                    {"total", obj.total},
            };
        }
    };

}
