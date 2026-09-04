//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ESM {

    struct GetObjectCountRequest {

        /**
         * @brief Bucket ERN
         */
        std::string ern{};

        /**
         * @brief Object key prefix
         */
        std::string prefix{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend GetObjectCountRequest tag_invoke(boost::json::value_to_tag<GetObjectCountRequest>, boost::json::value const &v) {
            GetObjectCountRequest r;
            r.ern = Core::GetStringValue(v, "ern");
            r.prefix = Core::GetStringValue(v, "prefix");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, GetObjectCountRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"prefix", obj.prefix},
            };
        }
    };

}