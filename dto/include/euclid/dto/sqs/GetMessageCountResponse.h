//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>

namespace Euclid::Dto::SQS {

    struct GetMessageCountResponse : BaseDto {

        /**
         * @brief Queue ERN
         */
        std::string ern{};

        /**
         * @brief Total number of messages available
         */
        long available{};

        /**
         * @brief Total number of messages delayed
         */
        long delayed{};

        /**
         * @brief Total number of messages invisible
         */
        long invisible{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend GetMessageCountResponse tag_invoke(boost::json::value_to_tag<GetMessageCountResponse>, boost::json::value const &v) {
            GetMessageCountResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.ern = Core::GetStringValue(v, "ern");
            r.available = Core::GetLongValue(v, "available");
            r.delayed = Core::GetLongValue(v, "delayed");
            r.invisible = Core::GetLongValue(v, "invisible");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, GetMessageCountResponse const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"available", obj.available},
                    {"delayed", obj.delayed},
                    {"invisible", obj.invisible},
            };
        }
    };

}