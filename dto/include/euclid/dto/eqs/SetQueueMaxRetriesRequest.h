// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 11/1/26.
//

#pragma once

// C++ includes
#include <string>

// Boost includes
#include <boost/json.hpp>

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EQS {

    /**
     * @brief Sets how many times a message may be received before it is moved to the queue's dead
     * letter queue.
     *
     * @par
     * Stored as the queue's maxReceiveCount - "maxRetries" is the name create-queue gives the same
     * figure. It only does anything on a queue that has a dead letter queue: without one, a message
     * received past the limit simply stays where it is. Messages already in the queue keep the
     * receive count they have, and are measured against the new limit on their next receive.
     */
    struct SetQueueMaxRetriesRequest {

        /**
         * @brief Queue ERN
         */
        std::string ern{};

        /**
         * @brief Maximal receive count, 1 to 1000.
         */
        long maxRetries{};

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
        static SetQueueMaxRetriesRequest fromJson(const std::string &json) {
            return boost::json::value_to<SetQueueMaxRetriesRequest>(Core::ParseJsonString(json));
        }

    private:

        friend SetQueueMaxRetriesRequest tag_invoke(boost::json::value_to_tag<SetQueueMaxRetriesRequest>, boost::json::value const &v) {
            SetQueueMaxRetriesRequest r;
            r.ern = Core::GetStringValue(v, "ern");
            r.maxRetries = Core::GetLongValue(v, "maxRetries");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, SetQueueMaxRetriesRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"maxRetries", obj.maxRetries},
            };
        }
    };

}
