//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ENS {

    struct PublishMessageResponse {

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

    private:

        friend PublishMessageResponse tag_invoke(boost::json::value_to_tag<PublishMessageResponse>, boost::json::value const &v) {
            PublishMessageResponse r;
            r.messageId = Core::GetStringValue(v, "messageId");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, PublishMessageResponse const &obj) {
            jv = {
                    {"messageId", obj.messageId},
            };
        }
    };

}