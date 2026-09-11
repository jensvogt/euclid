// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/10/26.
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
     * @brief Sets how long a topic keeps the messages published to it.
     */
    struct SetTopicRetentionRequest {

        /**
         * @brief Topic ERN
         */
        std::string ern;

        /**
         * @brief How long a message published to this topic is kept, in seconds.
         *
         * Zero means the topic has no period of its own and follows
         * euclid.modules.ens.retention-period, which is where a topic starts out.
         */
        long retentionPeriod{0};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static SetTopicRetentionRequest fromJson(const std::string &json) {
            return boost::json::value_to<SetTopicRetentionRequest>(Core::ParseJsonString(json));
        }

    private:

        friend SetTopicRetentionRequest tag_invoke(boost::json::value_to_tag<SetTopicRetentionRequest>, boost::json::value const &v) {
            SetTopicRetentionRequest r;
            r.ern = Core::GetStringValue(v, "ern");
            r.retentionPeriod = Core::GetLongValue(v, "retentionPeriod");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, SetTopicRetentionRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"retentionPeriod", obj.retentionPeriod},
            };
        }
    };

}
