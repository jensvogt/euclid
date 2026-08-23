//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>

namespace Euclid::Dto::SQS {

    struct GetQueueErnResponse : BaseDto {

        /**
         * @brief Queue ERN
         */
        std::string ern{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend GetQueueErnResponse tag_invoke(boost::json::value_to_tag<GetQueueErnResponse>, boost::json::value const &v) {
            GetQueueErnResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.ern = Core::GetStringValue(v, "ern");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, GetQueueErnResponse const &obj) {
            jv = {
                    {"metadata", boost::json::value_from(static_cast<const BaseDto &>(obj))},
                    {"ern", obj.ern},
            };
        }
    };

}