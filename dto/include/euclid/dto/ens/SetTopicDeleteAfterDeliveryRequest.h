// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <string>

// Boost includes
#include <boost/json.hpp>

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ENS {

    /**
     * @brief Sets whether a topic deletes a message once it has been handed to every subscription,
     * or keeps it for the retention period as a topic does by default.
     */
    struct SetTopicDeleteAfterDeliveryRequest {

        /**
         * @brief Topic ERN
         */
        std::string ern;

        /**
         * @brief True to delete each message once it has been handed to every subscription; false
         * to keep it for the retention period, which is where every topic starts out.
         *
         * @par
         * A message on a topic with no subscription is kept either way: it has been handed to
         * nobody. What a topic deletes is gone for resend-messages too.
         */
        bool deleteAfterDelivery{false};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static SetTopicDeleteAfterDeliveryRequest fromJson(const std::string &json) {
            return boost::json::value_to<SetTopicDeleteAfterDeliveryRequest>(Core::ParseJsonString(json));
        }

    private:

        friend SetTopicDeleteAfterDeliveryRequest tag_invoke(boost::json::value_to_tag<SetTopicDeleteAfterDeliveryRequest>,
                                                             boost::json::value const &v) {
            SetTopicDeleteAfterDeliveryRequest r;
            r.ern = Core::GetStringValue(v, "ern");
            r.deleteAfterDelivery = Core::GetBoolValue(v, "deleteAfterDelivery");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, SetTopicDeleteAfterDeliveryRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"deleteAfterDelivery", obj.deleteAfterDelivery},
            };
        }
    };

}
