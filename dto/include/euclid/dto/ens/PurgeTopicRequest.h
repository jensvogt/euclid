// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ENS {

    struct PurgeTopicRequest {

        /**
         * @brief ERN of the topic
         */
        std::string ern;

        /**
         * @brief Whether to answer before the topic has actually been emptied.
         *
         * @par
         * A topic that has been publishing for a while holds whatever its retention period let it
         * keep, and removing that takes as long as it takes - longer than the gateway's backend
         * timeout, and far longer than a client's patience. Emptying it inline means the caller
         * waits for all of it and is handed a timeout anyway, with the purge carrying on invisibly
         * behind the abandoned request. Asking for it asynchronously makes that honest: the answer
         * is "accepted", and says how many messages were there when it was given.
         *
         * @par
         * The same work either way, and nothing is resumed: a purge interrupted halfway has
         * removed some of the topic, and asking again removes the rest. EQS's PurgeQueueRequest
         * makes the same trade for the same reason.
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
        static PurgeTopicRequest fromJson(const std::string &json) {
            return boost::json::value_to<PurgeTopicRequest>(Core::ParseJsonString(json));
        }

    private:

        friend PurgeTopicRequest tag_invoke(boost::json::value_to_tag<PurgeTopicRequest>, boost::json::value const &v) {
            PurgeTopicRequest r;
            r.ern = Core::GetStringValue(v, "ern");
            r.async = Core::GetBoolValue(v, "async");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, PurgeTopicRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"async", obj.async},
            };
        }
    };
}