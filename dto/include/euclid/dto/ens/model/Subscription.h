// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/27/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::ENS {

    using Json::tag_invoke;

    using std::chrono::system_clock;

    struct Subscription {

        /**
         * @brief Euclid resource name of the subscription
         */
        std::string ern;

        /**
         * @brief ERN of the topic messages are published to
         */
        std::string sourceErn;

        /**
         * @brief Delivery protocol
         */
        std::string type;

        /**
         * @brief ERN of the delivery target
         */
        std::string targetErn;

        /**
         * @brief Creation date
         */
        system_clock::time_point created;

        /**
         * @brief Last modification date
         */
        system_clock::time_point modified;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]]
        static Subscription fromJson(const std::string &json) {
            return boost::json::value_to<Subscription>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(Subscription, (), (ern, sourceErn, type, targetErn, created, modified))

}// namespace Euclid::Dto::ENS
