//
// Created by vogje01 on 8/19/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::SQS {

    struct GetMessageMetadataRequest {

        /**
         * @brief Message ID
         */
        std::string messageId{};

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
        static GetMessageMetadataRequest fromJson(const std::string &json) {
            return boost::json::value_to<GetMessageMetadataRequest>(Core::ParseJsonString(json));
        }

    private:

        friend GetMessageMetadataRequest tag_invoke(boost::json::value_to_tag<GetMessageMetadataRequest>, boost::json::value const &v) {
            GetMessageMetadataRequest r;
            r.messageId = Core::GetStringValue(v, "messageId");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, GetMessageMetadataRequest const &obj) {
            jv = {
                    {"messageId", obj.messageId},
            };
        }
    };

}
