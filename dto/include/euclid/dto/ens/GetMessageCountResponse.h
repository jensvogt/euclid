//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ENS {

    struct GetMessageCountResponse {

        /**
         * @brief Topic ERN
         */
        std::string ern{};

        /**
         * @brief Total number of messages available
         */
        long available{};

        /**
         * @brief Total number of messages send
         */
        long send{};

        /**
         * @brief Total number of messages resend
         */
        long resend{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend GetMessageCountResponse tag_invoke(boost::json::value_to_tag<GetMessageCountResponse>, boost::json::value const &v) {
            GetMessageCountResponse r;
            r.ern = Core::GetStringValue(v, "ern");
            r.available = Core::GetLongValue(v, "available");
            r.send = Core::GetLongValue(v, "send");
            r.resend = Core::GetLongValue(v, "resend");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, GetMessageCountResponse const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"available", obj.available},
                    {"send", obj.send},
                    {"resend", obj.resend},
            };
        }
    };

}