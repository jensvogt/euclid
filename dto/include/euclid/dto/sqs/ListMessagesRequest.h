//
// Created by vogje01 on 8/19/26.
//

#pragma once

// C++ includes
#include <string>

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::SQS {

    struct ListMessagesRequest {

        /**
         * @brief ERN of the queue whose messages are listed
         */
        std::string queueErn{};

        /**
         * @brief Page size
         */
        long pageSize = 10;

        /**
         * @brief Page index
         */
        long pageIndex = 0;

        /**
         * @brief Sorting column
         */
        std::string sortColumn = "created";

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend ListMessagesRequest tag_invoke(boost::json::value_to_tag<ListMessagesRequest>, boost::json::value const &v) {
            ListMessagesRequest r;
            r.queueErn = Core::GetStringValue(v, "queueErn");
            r.pageSize = Core::GetLongValue(v, "pageSize");
            r.pageIndex = Core::GetLongValue(v, "pageIndex");
            r.sortColumn = Core::GetStringValue(v, "sortColumn");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ListMessagesRequest const &obj) {
            jv = {
                    {"queueErn", obj.queueErn},
                    {"pageSize", obj.pageSize},
                    {"pageIndex", obj.pageIndex},
                    {"sortColumn", obj.sortColumn},
            };
        }
    };

}
