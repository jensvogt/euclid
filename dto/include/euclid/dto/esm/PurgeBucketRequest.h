//
// Created by vogje01 on 8/24/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ESM {

    struct PurgeBucketRequest {

        /**
         * @brief ERN of the bucket
         */
        std::string ern;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]]
        std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]]
        static PurgeBucketRequest fromJson(const std::string &json) {
            return boost::json::value_to<PurgeBucketRequest>(Core::ParseJsonString(json));
        }

    private:

        friend PurgeBucketRequest tag_invoke(boost::json::value_to_tag<PurgeBucketRequest>, boost::json::value const &v) {
            PurgeBucketRequest r;
            r.ern = Core::GetStringValue(v, "ern");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, PurgeBucketRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
            };
        }
    };
}// namespace Euclid::Dto::ESM
