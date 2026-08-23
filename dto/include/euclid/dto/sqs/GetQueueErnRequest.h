//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::SQS {

    struct GetQueueErnRequest {

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

        friend GetQueueErnRequest tag_invoke(boost::json::value_to_tag<GetQueueErnRequest>, boost::json::value const &v) {
            GetQueueErnRequest r;
            r.name = Core::GetStringValue(v, "name");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, GetQueueErnRequest const &obj) {
            jv = {
                    {"name", obj.name},
            };
        }
    };

}