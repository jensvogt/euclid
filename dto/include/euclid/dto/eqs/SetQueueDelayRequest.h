// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/12/26.
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
     * @brief Sets how long a message sent to a queue is held back before it can be received.
     *
     * @par
     * Applies to what is sent from here on. A message already waiting keeps the moment it was
     * given when it arrived - its delay was turned into a timestamp then, and changing the queue's
     * figure does not go back and move it.
     */
    struct SetQueueDelayRequest {

        /**
         * @brief Queue ERN
         */
        std::string ern{};

        /**
         * @brief Delay in seconds, zero for none.
         */
        long delay{};

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
        static SetQueueDelayRequest fromJson(const std::string &json) {
            return boost::json::value_to<SetQueueDelayRequest>(Core::ParseJsonString(json));
        }

    private:

        friend SetQueueDelayRequest tag_invoke(boost::json::value_to_tag<SetQueueDelayRequest>, boost::json::value const &v) {
            SetQueueDelayRequest r;
            r.ern = Core::GetStringValue(v, "ern");
            r.delay = Core::GetLongValue(v, "delay");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, SetQueueDelayRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"delay", obj.delay},
            };
        }
    };

}
