#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EAM {

    struct ListNamespacesRequest {

        /**
         * @brief Only namespaces belonging to this account are returned
         */
        std::string accountId;

        /**
         * @brief Namespace name prefix
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
         * @brief Sorting column
         */
        std::string sortColumn = "name";

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend ListNamespacesRequest tag_invoke(boost::json::value_to_tag<ListNamespacesRequest>, boost::json::value const &v) {
            ListNamespacesRequest r;
            r.accountId = Core::GetStringValue(v, "accountId");
            r.prefix = Core::GetStringValue(v, "prefix");
            r.pageSize = Core::GetLongValue(v, "pageSize");
            r.pageIndex = Core::GetLongValue(v, "pageIndex");
            r.sortColumn = Core::GetStringValue(v, "sortColumn");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ListNamespacesRequest const &obj) {
            jv = {
                    {"accountId", obj.accountId},
                    {"prefix", obj.prefix},
                    {"pageSize", obj.pageSize},
                    {"pageIndex", obj.pageIndex},
                    {"sortColumn", obj.sortColumn},
            };
        }
    };

}
