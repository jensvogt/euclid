//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::SQS {

    struct PurgeQueueRequest {

        /**
         * @brief ERN of the queue
         */
        std::string ern;

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

    private:

        friend PurgeQueueRequest tag_invoke(boost::json::value_to_tag<PurgeQueueRequest>, boost::json::value const &v) {
            PurgeQueueRequest r;
            r.ern = Core::GetStringValue(v, "ern");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, PurgeQueueRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
            };
        }
    };
}
