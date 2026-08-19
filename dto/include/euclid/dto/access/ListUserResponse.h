//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <string>
#include <vector>

// Euclid includes
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/access/model/User.h>

namespace Euclid::Dto::Access {

    struct ListUserResponse : BaseDto {

        /**
         * @brief User ID prefix
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
                    {"metadata", boost::json::value_from(static_cast<const BaseDto &>(obj))},
                    {"users", boost::json::value_from(obj.users)},
                    {"total", obj.total},
            };
        }
    };

}