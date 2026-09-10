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
     * @brief Reads a table's items without regard to their key.
     */
    struct ScanRequest : BaseDto {

        /**
         * @brief Table to scan.
         */
        std::string table;

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

        friend ScanRequest tag_invoke(boost::json::value_to_tag<ScanRequest>, boost::json::value const &v) {
            ScanRequest r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.table = Core::GetStringValue(v, "table");
            r.pageSize = Core::GetLongValue(v, "pageSize");
            r.pageIndex = Core::GetLongValue(v, "pageIndex");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ScanRequest const &obj) {
            jv = {
                    {"metadata", boost::json::value_from(static_cast<const BaseDto &>(obj))},
                    {"table", obj.table},
                    {"pageSize", obj.pageSize},
                    {"pageIndex", obj.pageIndex},
            };
        }
    };
}
