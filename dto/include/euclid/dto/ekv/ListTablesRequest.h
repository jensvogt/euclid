//
// Created by vogje01 on 9/10/26.
//

#pragma once

// C++ includes
#include <string>
#include <vector>

// Boost includes
#include <boost/json.hpp>

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>

namespace Euclid::Dto::EKV {

    /**
     * @brief Lists the account's tables.
     */
    struct ListTablesRequest : BaseDto {

        /**
         * @brief Only tables whose name starts with this; empty matches all.
         */
        std::string prefix;

        /**
         * @brief Most to return; 0 means no limit.
         */
        long pageSize{0};

        /**
         * @brief Zero-based page, applied when pageSize is set.
         */
        long pageIndex{0};

        /**
         * @brief Field to sort by; empty sorts by name.
         */
        std::string sortColumn;

        /**
         * @brief "asc" or "desc".
         */
        std::string sortDirection;

        /**
         * @brief Serializes this to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend ListTablesRequest tag_invoke(boost::json::value_to_tag<ListTablesRequest>, boost::json::value const &v) {
            ListTablesRequest r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.prefix = Core::GetStringValue(v, "prefix");
            r.pageSize = Core::GetLongValue(v, "pageSize");
            r.pageIndex = Core::GetLongValue(v, "pageIndex");
            r.sortColumn = Core::GetStringValue(v, "sortColumn");
            r.sortDirection = Core::GetStringValue(v, "sortDirection");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ListTablesRequest const &obj) {
            jv = {
                    {"metadata", boost::json::value_from(static_cast<const BaseDto &>(obj))},
                    {"prefix", obj.prefix},
                    {"pageSize", obj.pageSize},
                    {"pageIndex", obj.pageIndex},
                    {"sortColumn", obj.sortColumn},
                    {"sortDirection", obj.sortDirection},
            };
        }
    };
}
