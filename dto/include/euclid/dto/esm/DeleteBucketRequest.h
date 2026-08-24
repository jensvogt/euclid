//
// Created by vogje01 on 8/23/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ESM {

    struct DeleteBucketRequest {

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
        static DeleteBucketRequest fromJson(const std::string &json) {
            return boost::json::value_to<DeleteBucketRequest>(Core::ParseJsonString(json));
        }

    private:

        friend DeleteBucketRequest tag_invoke(boost::json::value_to_tag<DeleteBucketRequest>, boost::json::value const &v) {
            DeleteBucketRequest r;
            r.ern = Core::GetStringValue(v, "ern");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, DeleteBucketRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
            };
        }
    };
}// namespace Euclid::Dto::ESM
