//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ESM {

    struct DeleteBucketTagRequest {

        /**
         * @brief Queue ERN
         */
        std::string queueErn;

        /**
         * @brief Key
         */
        std::string key;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static DeleteBucketTagRequest fromJson(const std::string &json) {
            return boost::json::value_to<DeleteBucketTagRequest>(Core::ParseJsonString(json));
        }

    private:

        friend DeleteBucketTagRequest tag_invoke(boost::json::value_to_tag<DeleteBucketTagRequest>, boost::json::value const &v) {
            DeleteBucketTagRequest r;
            r.queueErn = Core::GetStringValue(v, "ern");
            r.key = Core::GetStringValue(v, "key");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, DeleteBucketTagRequest const &obj) {
            jv = {
                    {"ern", obj.queueErn},
                    {"key", obj.key},
            };
        }
    };
}