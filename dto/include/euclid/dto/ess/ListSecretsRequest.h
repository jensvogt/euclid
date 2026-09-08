//
// Created by vogje01 on 9/8/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ESS {

    struct ListSecretsRequest {

        /**
         * @brief Secret name prefix
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
         * @brief Sorting column. "rotated" sorts by when each value was last changed, which is
         * what answers which secrets nobody has rotated in a year.
         */
        std::string sortColumn = "name";

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

        friend ListSecretsRequest tag_invoke(boost::json::value_to_tag<ListSecretsRequest>, boost::json::value const &v) {
            ListSecretsRequest r;
            r.prefix = Core::GetStringValue(v, "prefix");
            r.pageSize = Core::GetLongValue(v, "pageSize");
            r.pageIndex = Core::GetLongValue(v, "pageIndex");
            r.sortColumn = Core::GetStringValue(v, "sortColumn");
            r.sortDirection = Core::GetStringValue(v, "sortDirection");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ListSecretsRequest const &obj) {
            jv = {
                    {"prefix", obj.prefix},
                    {"pageSize", obj.pageSize},
                    {"pageIndex", obj.pageIndex},
                    {"sortColumn", obj.sortColumn},
                    {"sortDirection", obj.sortDirection},
            };
        }
    };

}// namespace Euclid::Dto::ESS
