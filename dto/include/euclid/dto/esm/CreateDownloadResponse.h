//
// Created by vogje01 on 8/24/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>

namespace Euclid::Dto::ESM {

    struct CreateDownloadResponse : BaseDto {

        /**
         * @brief Download ID (UUID) identifying the multipart download
         */
        std::string downloadId;

        /**
         * @brief ERN of the bucket the object is stored in
         */
        std::string bucketErn;

        /**
         * @brief Key (path) of the object being downloaded within the bucket
         */
        std::string key;

        /**
         * @brief Euclid resource name of the object being downloaded
         */
        std::string ern;

        /**
         * @brief Object size in bytes, needed by the caller to plan how many parts to request
         */
        long size = 0;

        /**
         * @brief MIME content type of the object
         */
        std::string contentType;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend CreateDownloadResponse tag_invoke(boost::json::value_to_tag<CreateDownloadResponse>, boost::json::value const &v) {
            CreateDownloadResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.downloadId = Core::GetStringValue(v, "downloadId");
            r.bucketErn = Core::GetStringValue(v, "bucketErn");
            r.key = Core::GetStringValue(v, "key");
            r.ern = Core::GetStringValue(v, "ern");
            r.size = Core::GetLongValue(v, "size");
            r.contentType = Core::GetStringValue(v, "contentType");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, CreateDownloadResponse const &obj) {
            jv = {
                    {"downloadId", obj.downloadId},
                    {"bucketErn", obj.bucketErn},
                    {"key", obj.key},
                    {"ern", obj.ern},
                    {"size", obj.size},
                    {"contentType", obj.contentType},
            };
        }
    };
}// namespace Euclid::Dto::ESM
