//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ENS {

    struct GetTopicErnRequest {

        /**
         * @brief Queue name
         */
        std::string name{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend GetTopicErnRequest tag_invoke(boost::json::value_to_tag<GetTopicErnRequest>, boost::json::value const &v) {
            GetTopicErnRequest r;
            r.name = Core::GetStringValue(v, "name");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, GetTopicErnRequest const &obj) {
            jv = {
                    {"name", obj.name},
            };
        }
    };

}