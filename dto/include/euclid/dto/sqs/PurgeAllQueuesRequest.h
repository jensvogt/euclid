//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::SQS {

    struct PurgeAllQueuesRequest {

        /**
         * @brief Region of the queues
         */
        std::string region;

        /**
         * @brief Account ID  of the queues
         */
        std::string accountId;

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
        static PurgeAllQueuesRequest fromJson(const std::string &json) {
            return boost::json::value_to<PurgeAllQueuesRequest>(Core::ParseJsonString(json));
        }

    private:

        friend PurgeAllQueuesRequest tag_invoke(boost::json::value_to_tag<PurgeAllQueuesRequest>, boost::json::value const &v) {
            PurgeAllQueuesRequest r;
            r.region = Core::GetStringValue(v, "region");
            r.accountId = Core::GetStringValue(v, "accountId");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, PurgeAllQueuesRequest const &obj) {
            jv = {
                    {"region", obj.region},
                    {"accountId", obj.accountId},
            };
        }
    };
}