//
// Created by vogje01 on 8/27/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ENS {

    struct SubscribeResponse {

        /**
         * @brief ERN of the newly created subscription.
         */
        std::string ern;

        /**
         * @brief ERN of the topic messages are published to.
         */
        std::string sourceErn;

        /**
         * @brief Delivery protocol.
         */
        std::string type;

        /**
         * @brief ERN of the delivery target.
         */
        std::string targetErn;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend SubscribeResponse tag_invoke(boost::json::value_to_tag<SubscribeResponse>, boost::json::value const &v) {
            SubscribeResponse r;
            r.ern = Core::GetStringValue(v, "ern");
            r.sourceErn = Core::GetStringValue(v, "sourceErn");
            r.type = Core::GetStringValue(v, "type");
            r.targetErn = Core::GetStringValue(v, "targetErn");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, SubscribeResponse const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"sourceErn", obj.sourceErn},
                    {"type", obj.type},
                    {"targetErn", obj.targetErn},
            };
        }
    };
}
