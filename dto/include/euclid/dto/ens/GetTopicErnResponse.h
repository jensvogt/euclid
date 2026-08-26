//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>

namespace Euclid::Dto::ENS {

    struct GetTopicErnResponse {

        /**
         * @brief Topic name
         */
        std::string name{};

        /**
         * @brief Topic ERN
         */
        std::string ern{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend GetTopicErnResponse tag_invoke(boost::json::value_to_tag<GetTopicErnResponse>, boost::json::value const &v) {
            GetTopicErnResponse r;
            r.name = Core::GetStringValue(v, "name");
            r.ern = Core::GetStringValue(v, "ern");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, GetTopicErnResponse const &obj) {
            jv = {
                    {"name", obj.name},
                    {"ern", obj.ern},
            };
        }
    };

}