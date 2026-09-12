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
     * @brief Sets the largest message a queue accepts.
     *
     * @par
     * Applies to what is sent from here on. A message already in the queue was measured against
     * the limit in force when it arrived and is not measured again.
     */
    struct SetQueueMaxMessageLengthRequest {

        /**
         * @brief Queue ERN
         */
        std::string ern{};

        /**
         * @brief The largest message the queue accepts, in bytes.
         *
         * @par
         * Zero is not "accept nothing", it is "carry no limit of your own": a send is then
         * measured against Entity::EQS::kDefaultMaxMessageLength, which is the same thing a queue
         * created before the limit meant anything is measured against. See
         * Entity::EQS::EffectiveMaxMessageLength().
         */
        long maxMessageLength{};

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
        static SetQueueMaxMessageLengthRequest fromJson(const std::string &json) {
            return boost::json::value_to<SetQueueMaxMessageLengthRequest>(Core::ParseJsonString(json));
        }

    private:

        friend SetQueueMaxMessageLengthRequest tag_invoke(boost::json::value_to_tag<SetQueueMaxMessageLengthRequest>, boost::json::value const &v) {
            SetQueueMaxMessageLengthRequest r;
            r.ern = Core::GetStringValue(v, "ern");
            r.maxMessageLength = Core::GetLongValue(v, "maxMessageLength");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, SetQueueMaxMessageLengthRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"maxMessageLength", obj.maxMessageLength},
            };
        }
    };

}
