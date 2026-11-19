// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/30/26.
//

#pragma once

// C++ includes
#include <array>
#include <string_view>

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::EKM {

    using Json::tag_invoke;

    struct DeleteKeyRequest {

        /**
         * @brief ID of the key to delete (as returned by create-key)
         */
        std::string keyId;

        /**
         * @brief Number of days to wait before the key is permanently deleted
         */
        long pendingWindowInDays = 7;

        /**
         * @brief The members that keep their initial value when the JSON leaves them out, rather than
         * becoming zero, as the other members do. The delete-key handler refuses a window below a
         * day, so a request that omits this still gets its seven-day grace period.
         */
        static constexpr std::array<std::string_view, 1> kJsonDefaults{"pendingWindowInDays"};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static DeleteKeyRequest fromJson(const std::string &json) {
            return boost::json::value_to<DeleteKeyRequest>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(DeleteKeyRequest, (), (keyId, pendingWindowInDays))
}
