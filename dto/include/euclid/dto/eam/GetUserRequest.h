// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/19/26.
//

#pragma once

// C++ includes
#include <string>

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EAM {

    /**
     * @brief Reads one user, by the id they are known by.
     *
     * @par
     * The user id rather than the ERN, because that is what everything else names a user with: a
     * grant's principal, an application's technical identity, the audit trail's userId column.
     */
    struct GetUserRequest {

        /**
         * @brief User id.
         */
        std::string userId;

        /**
         * @brief Serializes this to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend GetUserRequest tag_invoke(boost::json::value_to_tag<GetUserRequest>, boost::json::value const &v) {
            GetUserRequest r;
            r.userId = Core::GetStringValue(v, "userId");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, GetUserRequest const &obj) {
            jv = {
                    {"userId", obj.userId},
            };
        }
    };
}// namespace Euclid::Dto::EAM
