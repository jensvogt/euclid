// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::ENS {

    using Json::tag_invoke;

    struct GetMessageCountResponse {

        /**
         * @brief Topic ERN
         */
        std::string ern{};

        /**
         * @brief Total number of messages available
         */
        long available{};

        /**
         * @brief Total number of messages send
         */
        long send{};

        /**
         * @brief Total number of messages resend
         */
        long resend{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(GetMessageCountResponse, (), (ern, available, send, resend))

}