//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <string>

// Euclid include
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/eam/model/User.h>

namespace Euclid::Dto::EAM {

    struct RegisterResponse : BaseDto {

        /**
         * @brief User ID of the newly created user
         */
        User user;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend RegisterResponse tag_invoke(boost::json::value_to_tag<RegisterResponse>, boost::json::value const &v) {
            RegisterResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.user = boost::json::value_to<User>(v.at("user"));
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, RegisterResponse const &obj) {
            jv = {
                    {"metadata", boost::json::value_from(static_cast<const BaseDto &>(obj))},
                    {"user", boost::json::value_from(obj.user)},
            };
        }
    };

}