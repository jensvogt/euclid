//
// Created by vogje01 on 8/19/26.
//

#pragma once

// C++ includes
#include <string>
#include <vector>

// Euclid includes
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/eqs/model/Message.h>

namespace Euclid::Dto::EQS {

    struct ListMessagesResponse : BaseDto {

        /**
         * @brief Message list
         */
        std::vector<Message> messages;

        /**
         * @brief Total number of messages in the queue
         */
        long total{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend ListMessagesResponse tag_invoke(boost::json::value_to_tag<ListMessagesResponse>, boost::json::value const &v) {
            ListMessagesResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.messages = boost::json::value_to<std::vector<Message> >(v.at("messages"));
            r.total = Core::GetLongValue(v, "total");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ListMessagesResponse const &obj) {
            jv = {
                    {"messages", boost::json::value_from(obj.messages)},
                    {"total", obj.total},
            };
        }
    };

}
