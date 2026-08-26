//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ENS {

    struct GetMessageCountRequest {

        /**
         * @brief Topic ERN
         */
        std::string topicErn{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend GetMessageCountRequest tag_invoke(boost::json::value_to_tag<GetMessageCountRequest>, boost::json::value const &v) {
            GetMessageCountRequest r;
            r.topicErn = Core::GetStringValue(v, "ern");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, GetMessageCountRequest const &obj) {
            jv = {
                    {"ern", obj.topicErn},
            };
        }
    };

}