//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>

namespace Euclid::Dto::ENS {

    struct CreateTopicResponse {

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

        friend CreateTopicResponse tag_invoke(boost::json::value_to_tag<CreateTopicResponse>, boost::json::value const &v) {
            CreateTopicResponse r;
            r.name = Core::GetStringValue(v, "name");
            r.ern = Core::GetStringValue(v, "ern");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, CreateTopicResponse const &obj) {
            jv = {
                    {"name", obj.name},
                    {"ern", obj.ern},
            };
        }
    };
}