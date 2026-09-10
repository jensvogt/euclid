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
     * @brief Reads the items of one partition, in sort-key order.
     */
    struct QueryRequest : BaseDto {

        /**
         * @brief Table to query.
         */
        std::string table;

        /**
         * @brief The partition key's value.
         */
        boost::json::value partitionKey;

        /**
         * @brief How to narrow by sort key: eq, lt, le, gt, ge, between, begins-with; empty takes the whole partition.
         */
        std::string sortOperator;

        /**
         * @brief What to compare the sort key against; the lower bound for between.
         */
        boost::json::value sortValue;

        /**
         * @brief The upper bound, for between only.
         */
        boost::json::value sortUpper;

        /**
         * @brief Whether to read in ascending sort-key order.
         */
        bool forward{true};

        /**
         * @brief Most to return; 0 means no limit.
         */
        long pageSize{0};

        /**
         * @brief Zero-based page, applied when pageSize is set.
         */
        long pageIndex{0};

        /**
         * @brief Serializes this to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend QueryRequest tag_invoke(boost::json::value_to_tag<QueryRequest>, boost::json::value const &v) {
            QueryRequest r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.table = Core::GetStringValue(v, "table");
            if (v.is_object()) {
                if (const auto *found = v.as_object().if_contains("partitionKey"); found != nullptr) r.partitionKey = *found;
            }
            r.sortOperator = Core::GetStringValue(v, "sortOperator");
            if (v.is_object()) {
                if (const auto *found = v.as_object().if_contains("sortValue"); found != nullptr) r.sortValue = *found;
            }
            if (v.is_object()) {
                if (const auto *found = v.as_object().if_contains("sortUpper"); found != nullptr) r.sortUpper = *found;
            }
            r.forward = Core::GetBoolValue(v, "forward");
            r.pageSize = Core::GetLongValue(v, "pageSize");
            r.pageIndex = Core::GetLongValue(v, "pageIndex");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, QueryRequest const &obj) {
            jv = {
                    {"metadata", boost::json::value_from(static_cast<const BaseDto &>(obj))},
                    {"table", obj.table},
                    {"partitionKey", obj.partitionKey},
                    {"sortOperator", obj.sortOperator},
                    {"sortValue", obj.sortValue},
                    {"sortUpper", obj.sortUpper},
                    {"forward", obj.forward},
                    {"pageSize", obj.pageSize},
                    {"pageIndex", obj.pageIndex},
            };
        }
    };
}
