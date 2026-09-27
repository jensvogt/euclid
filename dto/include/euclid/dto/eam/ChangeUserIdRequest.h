// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <string>

// Boost includes
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EAM {

    /**
     * @brief Gives a user a different user ID.
     *
     * @par
     * Both ids are named outright, and the old one is not defaulted to the caller's own the way
     * ChangePasswordRequest's is. Renaming is an administrator's action on somebody, and a request
     * that could mean "me" by saying nothing is one keystroke from renaming the wrong account.
     */
    struct ChangeUserIdRequest {

        /**
         * @brief The user being renamed.
         */
        std::string userId;

        /**
         * @brief The id they should have from now on. Refused if another user holds it.
         */
        std::string newUserId;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend ChangeUserIdRequest tag_invoke(boost::json::value_to_tag<ChangeUserIdRequest>, boost::json::value const &v) {
            ChangeUserIdRequest r;
            r.userId = Core::GetStringValue(v, "userId");
            r.newUserId = Core::GetStringValue(v, "newUserId");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ChangeUserIdRequest const &obj) {
            jv = {
                    {"userId", obj.userId},
                    {"newUserId", obj.newUserId},
            };
        }
    };

}
