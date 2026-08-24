//
// Created by vogje01 on 8/24/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ESM {

    struct CreateDownloadRequest {

        /**
         * @brief ERN of the bucket the object is stored in
         */
        std::string bucketErn;

        /**
         * @brief Key (path) of the object being downloaded within the bucket
         */
        std::string key;

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static CreateDownloadRequest fromJson(const std::string &json) {
            return boost::json::value_to<CreateDownloadRequest>(Core::ParseJsonString(json));
        }

    private:

        friend CreateDownloadRequest tag_invoke(boost::json::value_to_tag<CreateDownloadRequest>, boost::json::value const &v) {
            CreateDownloadRequest r;
            r.bucketErn = Core::GetStringValue(v, "bucketErn");
            r.key = Core::GetStringValue(v, "key");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, CreateDownloadRequest const &obj) {
            jv = {
                    {"bucketErn", obj.bucketErn},
                    {"key", obj.key},
            };
        }
    };
}// namespace Euclid::Dto::ESM
