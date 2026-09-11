// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <vector>

// Euclid includes
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/eam/model/User.h>

namespace Euclid::Dto::EAM {

    struct ListUserResponse : BaseDto {

        /**
         * @brief Users
         */
        std::vector<User> users;

        /**
         * @brief total number of users
         */
        long total{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend ListUserResponse tag_invoke(boost::json::value_to_tag<ListUserResponse>, boost::json::value const &v) {
            ListUserResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.users = boost::json::value_to<std::vector<User> >(v.at("users"));
            r.total = Core::GetLongValue(v, "total");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ListUserResponse const &obj) {
            jv = {
                    {"users", boost::json::value_from(obj.users)},
                    {"total", obj.total},
            };
        }
    };

}