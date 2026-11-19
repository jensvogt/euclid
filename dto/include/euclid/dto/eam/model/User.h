// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <string>
#include <vector>

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto {

    using Json::tag_invoke;

    struct User {
        /**
         * @brief User ID
         */
        std::string userId;

        /**
         * @brief Euclid resource name
         */
        std::string ern;

        /**
         * @brief User password
         */
        std::string password;

        /**
         * @brief User email
         */
        std::string email;

        /**
         * @brief Account the user belongs to
         */
        std::string accountId;

        /**
         * @brief Region the user was registered in
         */
        std::string region;


        /**
         * @brief Created timestamp
         */
        std::chrono::system_clock::time_point created;

        /**
         * @brief Created timestamp
         */
        std::chrono::system_clock::time_point modified;
    };

    BOOST_DESCRIBE_STRUCT(User, (), (userId, ern, password, email, accountId, region, created, modified))
}