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
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, PurgeTopicRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
            };
        }
    };
}