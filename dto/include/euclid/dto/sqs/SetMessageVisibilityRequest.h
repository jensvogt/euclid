//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::SQS {

    struct SetMessageVisibilityRequest {

        /**
         * @brief Message ID
         */
        std::string messageId{};

        /**
         * @brief Visibility in seconds
         */
        long visibility;

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
        static SetMessageVisibilityRequest fromJson(const std::string &json) {
            return boost::json::value_to<SetMessageVisibilityRequest>(Core::ParseJsonString(json));
        }

    private:

        friend SetMessageVisibilityRequest tag_invoke(boost::json::value_to_tag<SetMessageVisibilityRequest>, boost::json::value const &v) {
            SetMessageVisibilityRequest r;
            r.messageId = Core::GetStringValue(v, "messageId");
            r.visibility = Core::GetLongValue(v, "visibility");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, SetMessageVisibilityRequest const &obj) {
            jv = {
                    {"messageId", obj.messageId},
                    {"visibility", obj.visibility},
            };
        }
    };

}