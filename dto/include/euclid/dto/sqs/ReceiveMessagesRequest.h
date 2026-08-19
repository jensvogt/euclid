//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::SQS {

    struct ReceiveMessagesRequest {

        /**
         * @brief Queue ERN
         */
        std::string ern{};

        /**
         * @brief Maximal message count
         */
        long maxCount = 10;

        /**
         * @brief Maximal wait time
         */
        long waitTime = 0;

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
        static ReceiveMessagesRequest fromJson(const std::string &json) {
            return boost::json::value_to<ReceiveMessagesRequest>(Core::ParseJsonString(json));
        }

    private:

        friend ReceiveMessagesRequest tag_invoke(boost::json::value_to_tag<ReceiveMessagesRequest>, boost::json::value const &v) {
            ReceiveMessagesRequest r;
            r.ern = Core::GetStringValue(v, "ern");
            r.maxCount = Core::GetLongValue(v, "maxCount");
            r.waitTime = Core::GetLongValue(v, "waitTime");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ReceiveMessagesRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"maxCount", obj.maxCount},
                    {"waitTime", obj.waitTime},
            };
        }
    };

}
