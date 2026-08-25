//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ESM {

    struct SetBucketTagRequest {

        /**
         * @brief Queue ERN
         */
        std::string bucketErn;

        /**
         * @brief Key
         */
        std::string key;

        /**
         * @brief Value
         */
        std::string value;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static SetBucketTagRequest fromJson(const std::string &json) {
            return boost::json::value_to<SetBucketTagRequest>(Core::ParseJsonString(json));
        }

    private:

        friend SetBucketTagRequest tag_invoke(boost::json::value_to_tag<SetBucketTagRequest>, boost::json::value const &v) {
            SetBucketTagRequest r;
            r.bucketErn = Core::GetStringValue(v, "ern");
            r.key = Core::GetStringValue(v, "key");
            r.value = Core::GetStringValue(v, "value");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, SetBucketTagRequest const &obj) {
            jv = {
                    {"ern", obj.bucketErn},
                    {"key", obj.key},
                    {"value", obj.value},
            };
        }
    };
}