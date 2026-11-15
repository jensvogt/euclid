// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/27/26.
//

#pragma once

// C++ includes
#include <vector>

// C++ includes
#include <array>
#include <string_view>

// Euclid includes
#include <euclid/dto/ens/model/Subscription.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::ENS {

    using Json::tag_invoke;

    struct ListSubscriptionsResponse {

        /**
         * @brief Subscription list
         */
        std::vector<Subscription> subscriptions;

        /**
         * @brief total number of subscriptions
         */
        long total{};

        /**
         * @brief The members that must be in the JSON: missing or null, reading throws.
         */
        static constexpr std::array<std::string_view, 1> kJsonRequired{"subscriptions"};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(ListSubscriptionsResponse, (), (subscriptions, total))

}
