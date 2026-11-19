// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/16/26.
//

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

    struct RegisterRequest {

        /**
         * @brief User ID
         */
        std::string userId;

        /**
         * @brief User password (plaintext, hashed before storage)
         */
        std::string password;

        /**
         * @brief User email
         */
        std::string email;

        /**
         * @brief Account the new user is registered into
         */
        std::string accountId;

        /**
         * @brief Region the new user is registered in
         */
        std::string region;

        /**
         * @brief Whether the new user should have administrator privileges.
         *
         * Ignored unless the caller is itself an administrator - the exception is the
         * very first user ever registered, which always becomes an administrator.
         */
        bool isAdmin{false};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(RegisterRequest, (), (userId, password, email, accountId, region, isAdmin))

}
