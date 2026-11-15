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

namespace Euclid::Dto::EQS {

    using Json::tag_invoke;

    struct PurgeAllQueuesRequest {

        /**
         * @brief Region of the queues
         */
        std::string region;

        /**
         * @brief Account ID  of the queues
         */
        std::string accountId;

        /**
         * @brief Name space of the queues; empty purges every namespace of the account
         */
        std::string nameSpace;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]]
        std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]]
        static PurgeAllQueuesRequest fromJson(const std::string &json) {
            return boost::json::value_to<PurgeAllQueuesRequest>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(PurgeAllQueuesRequest, (), (region, accountId, nameSpace))
}