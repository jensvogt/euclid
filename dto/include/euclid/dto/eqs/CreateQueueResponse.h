//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>

namespace Euclid::Dto::EQS {

    struct CreateQueueResponse : BaseDto {

        /**
         * @brief Name of the queue
         */
        std::string name;

        /**
         * @brief Euclid resource name
         */
        std::string ern;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend CreateQueueResponse tag_invoke(boost::json::value_to_tag<CreateQueueResponse>, boost::json::value const &v) {
            CreateQueueResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.name = Core::GetStringValue(v, "name");
            r.ern = Core::GetStringValue(v, "ern");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, CreateQueueResponse const &obj) {
            jv = {
                    //                    {"metadata", boost::json::value_from(static_cast<const BaseDto &>(obj))},
                    {"name", obj.name},
                    {"ern", obj.ern},
            };
        }
    };
}