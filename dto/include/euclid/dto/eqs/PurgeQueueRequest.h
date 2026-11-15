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

    struct PurgeQueueRequest {

        /**
         * @brief ERN of the queue
         */
        std::string ern;

        /**
         * @brief Whether to answer before the queue has actually been emptied.
         *
         * @par
         * A queue holding a million messages takes as long to empty as it takes - longer than the
         * gateway's backend timeout, and far longer than a client's patience. Emptying it inline
         * means the caller waits for all of it and is handed a timeout anyway, with the purge
         * carrying on invisibly behind the abandoned request. Asking for it asynchronously makes
         * that honest: the answer is "accepted", and says how many messages were there when it was
         * given.
         *
         * @par
         * The same work either way, and nothing is resumed: a purge interrupted halfway has
         * removed some of the queue, and asking again removes the rest.
         */
        bool async{false};

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
        static PurgeQueueRequest fromJson(const std::string &json) {
            return boost::json::value_to<PurgeQueueRequest>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(PurgeQueueRequest, (), (ern, async))
}
