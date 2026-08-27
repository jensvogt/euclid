//
// Created by vogje01 on 8/27/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ESM {

    struct ListSubscriptionsRequest {

        /**
         * @brief ERN of the bucket whose subscriptions are listed
         */
        std::string bucketErn{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend ListSubscriptionsRequest tag_invoke(boost::json::value_to_tag<ListSubscriptionsRequest>, boost::json::value const &v) {
            ListSubscriptionsRequest r;
            r.bucketErn = Core::GetStringValue(v, "bucketErn");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ListSubscriptionsRequest const &obj) {
            jv = {
                    {"bucketErn", obj.bucketErn},
            };
        }
    };

}
