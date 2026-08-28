//
// Created by vogje01 on 8/19/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ENS {

    struct ListMessagesRequest {

        /**
         * @brief ERN of the topic whose messages are listed
         */
        std::string topicErn{};

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
         * @brief Sort direction
         */
        std::string sortDirection = "asc";

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend ListMessagesRequest tag_invoke(boost::json::value_to_tag<ListMessagesRequest>, boost::json::value const &v) {
            ListMessagesRequest r;
            r.topicErn = Core::GetStringValue(v, "topicErn");
            r.pageSize = Core::GetLongValue(v, "pageSize");
            r.pageIndex = Core::GetLongValue(v, "pageIndex");
            r.sortColumn = Core::GetStringValue(v, "sortColumn");
            r.sortDirection = Core::GetStringValue(v, "sortDirection");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ListMessagesRequest const &obj) {
            jv = {
                    {"topicErn", obj.topicErn},
                    {"pageSize", obj.pageSize},
                    {"pageIndex", obj.pageIndex},
                    {"sortColumn", obj.sortColumn},
                    {"sortDirection", obj.sortDirection},
            };
        }
    };

}