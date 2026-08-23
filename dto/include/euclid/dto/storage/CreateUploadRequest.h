//
// Created by vogje01 on 8/23/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::Storage {

    struct CreateUploadRequest {

        /**
         * @brief ERN of the bucket the upload will be stored in
         */
        std::string bucketErn;

        /**
         * @brief Key (path) of the object being uploaded within the bucket
         */
        std::string key;

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static CreateUploadRequest fromJson(const std::string &json) {
            return boost::json::value_to<CreateUploadRequest>(Core::ParseJsonString(json));
        }

    private:

        friend CreateUploadRequest tag_invoke(boost::json::value_to_tag<CreateUploadRequest>, boost::json::value const &v) {
            CreateUploadRequest r;
            r.bucketErn = Core::GetStringValue(v, "bucketErn");
            r.key = Core::GetStringValue(v, "key");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, CreateUploadRequest const &obj) {
            jv = {
                    {"bucketErn", obj.bucketErn},
                    {"key", obj.key},
            };
        }
    };
}// namespace Euclid::Dto::Storage
