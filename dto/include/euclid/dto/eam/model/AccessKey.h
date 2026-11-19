// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/18/26.
//

#pragma once

// C++ includes
#include <string>

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::EAM {

    using Json::tag_invoke;

    /**
     * @brief Access key summary, as returned by list-access-keys.
     *
     * Deliberately omits secretAccessKey - the secret is only ever returned once, from
     * create-access-key, and is never stored or echoed back afterward.
     */
    struct AccessKey {

        /**
         * @brief Public identifier, e.g. "AKIA...".
         */
        std::string accessKeyId;

        /**
         * @brief Whether this key can currently be used to sign requests.
         */
        bool active{true};

        /**
         * @brief Creation timestamp, ISO8601.
         */
        std::string createdAt;
    };

    BOOST_DESCRIBE_STRUCT(AccessKey, (), (accessKeyId, active, createdAt))

}
