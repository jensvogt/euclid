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
#include <euclid/dto/eam/model/AuthorizationGap.h>

namespace Euclid::Dto::EAM {

    /**
     * @brief What shadow mode has recorded, most-seen first.
     */
    struct AuthorizationGapsResponse : BaseDto {

        /**
         * @brief The gaps, most-seen first - the one refused ten thousand times is the grant to
         * write first.
         */
        std::vector<AuthorizationGap> gaps;

        /**
         * @brief How many distinct gaps have been recorded in total.
         */
        long total{};

        /**
         * @brief What euclid.eam.authorization currently says, so a caller can tell an empty list
         * that means "nothing is missing" from one that means "nothing has been watching".
         */
        std::string mode;

        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend AuthorizationGapsResponse tag_invoke(boost::json::value_to_tag<AuthorizationGapsResponse>, boost::json::value const &v) {
            AuthorizationGapsResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            for (const auto &gap: v.at("gaps").as_array()) r.gaps.push_back(boost::json::value_to<AuthorizationGap>(gap));
            r.total = Core::GetLongValue(v, "total");
            r.mode = Core::GetStringValue(v, "mode");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, AuthorizationGapsResponse const &obj) {
            jv = {
                    {"metadata", boost::json::value_from(static_cast<const BaseDto &>(obj))},
                    {"gaps", boost::json::value_from(obj.gaps)},
                    {"total", obj.total},
                    {"mode", obj.mode},
            };
        }
    };

}
