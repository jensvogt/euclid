//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EQS {

    struct ListQueueRequest {

        /**
         * @brief User ID prefix
         */
        std::string prefix{};

        /**
         * @brief Page size
         */
        long pageSize = 10;

        /**
         * @brief Page index
         */
        long pageIndex = 0;

        /**
         * @brief Sorting columns
         */
        std::string sortColumn = "name";

        /**
         * @brief Sort direction
         */
        std::string sortDirection = "asc";

        /**
         * @brief Whether euclid's own internal queues are listed as well.
         *
         * Honoured for administrators only - see EqsServer's list-queues handler. Off by default,
         * so every existing caller keeps the listing it had.
         */
        bool includeInternal = false;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend ListQueueRequest tag_invoke(boost::json::value_to_tag<ListQueueRequest>, boost::json::value const &v) {
            ListQueueRequest r;
            r.prefix = Core::GetStringValue(v, "prefix");
            r.pageSize = Core::GetLongValue(v, "pageSize");
            r.pageIndex = Core::GetLongValue(v, "pageIndex");
            r.sortColumn = Core::GetStringValue(v, "sortColumn");
            r.sortDirection = Core::GetStringValue(v, "sortDirection");
            r.includeInternal = Core::GetBoolValue(v, "includeInternal");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ListQueueRequest const &obj) {
            jv = {
                    {"prefix", obj.prefix},
                    {"pageSize", obj.pageSize},
                    {"pageIndex", obj.pageIndex},
                    {"sortColumn", obj.sortColumn},
                    {"sortDirection", obj.sortDirection},
                    {"includeInternal", obj.includeInternal},
            };
        }
    };

}