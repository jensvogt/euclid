// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::EQS {

    using Json::tag_invoke;

    struct GetMessageCountResponse : BaseDto {

        /**
         * @brief Queue ERN
         */
        std::string ern{};

        /**
         * @brief Total number of messages available
         */
        long available{};

        /**
         * @brief Total number of messages delayed
         */
        long delayed{};

        /**
         * @brief Total number of messages invisible
         */
        long invisible{};

        /**
         * @brief Total number of messages, i.e. available + delayed + invisible
         */
        long total{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(GetMessageCountResponse, (BaseDto), (ern, available, delayed, invisible, total))

}