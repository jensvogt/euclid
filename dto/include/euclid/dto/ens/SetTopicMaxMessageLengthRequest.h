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

namespace Euclid::Dto::ENS {

    /**
     * @brief Sets the largest message a topic accepts.
     */
    struct SetTopicMaxMessageLengthRequest {

        /**
         * @brief Topic ERN
         */
        std::string ern;

        /**
         * @brief The largest message the topic accepts from now on, in bytes.
         *
         * @par
         * Applies to what is published after it: a message already in the topic is not re-checked
         * and is not removed by lowering this, because it was accepted under the rule that was in
         * force when it arrived.
         *
         * @par
         * Has to be positive. Zero would be a topic that accepts nothing at all, which is a
         * mistake rather than a configuration - stopping a topic is what says "take nothing for
         * now", and it says so reversibly.
         */
        long maxMessageLength{0};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static SetTopicMaxMessageLengthRequest fromJson(const std::string &json) {
            return boost::json::value_to<SetTopicMaxMessageLengthRequest>(Core::ParseJsonString(json));
        }

    private:

        friend SetTopicMaxMessageLengthRequest tag_invoke(boost::json::value_to_tag<SetTopicMaxMessageLengthRequest>, boost::json::value const &v) {
            SetTopicMaxMessageLengthRequest r;
            r.ern = Core::GetStringValue(v, "ern");
            r.maxMessageLength = Core::GetLongValue(v, "maxMessageLength");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, SetTopicMaxMessageLengthRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"maxMessageLength", obj.maxMessageLength},
            };
        }
    };

}
