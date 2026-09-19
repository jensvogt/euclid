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
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/eam/model/User.h>

namespace Euclid::Dto::EAM {

    /**
     * @brief One user, as list-users describes each of its own.
     *
     * @par
     * The same User model rather than a shape of its own, so that what a listing shows and what
     * this shows cannot drift apart - a field added to one is in the other by construction.
     */
    struct GetUserResponse : BaseDto {

        /**
         * @brief The user.
         */
        User user;

        /**
         * @brief Serializes this response to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend GetUserResponse tag_invoke(boost::json::value_to_tag<GetUserResponse>, boost::json::value const &v) {
            GetUserResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.user = boost::json::value_to<User>(v.at("user"));
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, GetUserResponse const &obj) {
            jv = {
                    {"user", boost::json::value_from(obj.user)},
            };
        }
    };
}// namespace Euclid::Dto::EAM
