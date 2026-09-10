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
     * @brief Removes one item by its key.
     */
    struct DeleteItemRequest : BaseDto {

        /**
         * @brief Table to remove from.
         */
        std::string table;

        /**
         * @brief The key attributes, as an object.
         */
        boost::json::value key;

        /**
         * @brief Serializes this to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend DeleteItemRequest tag_invoke(boost::json::value_to_tag<DeleteItemRequest>, boost::json::value const &v) {
            DeleteItemRequest r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.table = Core::GetStringValue(v, "table");
            if (v.is_object()) {
                if (const auto *found = v.as_object().if_contains("key"); found != nullptr) r.key = *found;
            }
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, DeleteItemRequest const &obj) {
            jv = {
                    {"metadata", boost::json::value_from(static_cast<const BaseDto &>(obj))},
                    {"table", obj.table},
                    {"key", obj.key},
            };
        }
    };
}
