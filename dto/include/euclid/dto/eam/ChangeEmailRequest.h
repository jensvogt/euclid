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
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::EAM {

    using Json::tag_invoke;

    /**
     * @brief Gives a user a different email address.
     *
     * @par
     * The user is named outright, as in ChangeUserIdRequest and not as in ChangePasswordRequest:
     * an email is a way in - login accepts it in place of the user id - so a request that could
     * mean "me" by saying nothing is one keystroke from pointing the wrong account at an address.
     */
    struct ChangeEmailRequest {

        /**
         * @brief The user whose address is changing.
         */
        std::string userId;

        /**
         * @brief The address they should have from now on. Refused if another user holds it.
         */
        std::string email;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(ChangeEmailRequest, (), (userId, email))

}
