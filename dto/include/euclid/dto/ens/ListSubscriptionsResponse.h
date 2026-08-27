//
// Created by vogje01 on 8/27/26.
//

#pragma once

// C++ includes
#include <vector>

// Euclid includes
#include <euclid/dto/ens/model/Subscription.h>

namespace Euclid::Dto::ENS {

    struct ListSubscriptionsResponse {

        /**
         * @brief Subscription list
         */
        std::vector<Subscription> subscriptions;

        /**
         * @brief total number of subscriptions
         */
        long total{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend ListSubscriptionsResponse tag_invoke(boost::json::value_to_tag<ListSubscriptionsResponse>, boost::json::value const &v) {
            ListSubscriptionsResponse r;
            r.subscriptions = boost::json::value_to<std::vector<Subscription> >(v.at("subscriptions"));
            r.total = Core::GetLongValue(v, "total");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ListSubscriptionsResponse const &obj) {
            jv = {
                    {"subscriptions", boost::json::value_from(obj.subscriptions)},
                    {"total", obj.total},
            };
        }
    };

}
