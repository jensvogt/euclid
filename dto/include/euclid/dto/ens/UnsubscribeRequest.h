//
// Created by vogje01 on 8/27/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ENS {

    struct UnsubscribeRequest {

        /**
         * @brief ERN of the subscription to delete.
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
        static UnsubscribeRequest fromJson(const std::string &json) {
            return boost::json::value_to<UnsubscribeRequest>(Core::ParseJsonString(json));
        }

    private:

        friend UnsubscribeRequest tag_invoke(boost::json::value_to_tag<UnsubscribeRequest>, boost::json::value const &v) {
            UnsubscribeRequest r;
            r.ern = Core::GetStringValue(v, "ern");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, UnsubscribeRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
            };
        }
    };
}
