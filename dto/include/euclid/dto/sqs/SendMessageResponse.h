//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>

namespace Euclid::Dto::SQS {

    struct SendMessageResponse : BaseDto {

        /**
         * @brief Message ID
         */
        std::string messageId{};

        /**
         * @brief MD5 sum of the message body
         */
        std::string md5Body{};

        /**
         * @brief MD5 sum of the message attributes
         */
        std::string md5Attributes{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend SendMessageResponse tag_invoke(boost::json::value_to_tag<SendMessageResponse>, boost::json::value const &v) {
            SendMessageResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.messageId = Core::GetStringValue(v, "messageId");
            r.md5Body = Core::GetStringValue(v, "md5Body");
            r.md5Attributes = Core::GetStringValue(v, "md5Attributes");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, SendMessageResponse const &obj) {
            jv = {
                    {"messageId", obj.messageId},
                    {"md5Body", obj.md5Body},
                    {"md5Attributes", obj.md5Attributes},
            };
        }
    };

}