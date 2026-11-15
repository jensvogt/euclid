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

    struct GetQueueMetadataResponse {

        /**
         * @brief Region of the queue
         */
        std::string region{};

        /**
         * @brief Account ID of the queue
         */
        std::string accountId{};

        /**
         * @brief Owner of the queue
         */
        std::string owner{};

        /**
         * @brief Namespace of queue
         */
        std::string nameSpace{};

        /**
         * @brief Queue name
         */
        std::string name{};

        /**
         * @brief Queue ERN
         */
        std::string ern{};

        /**
         * @brief Queue size in bytes
         */
        long size{};

        /**
         * @brief Total number of messages
         */
        long messages{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(GetQueueMetadataResponse, (), (region, accountId, owner, nameSpace, name, ern, size, messages))

}