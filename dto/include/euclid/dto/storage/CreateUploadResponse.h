//
// Created by vogje01 on 8/23/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>

namespace Euclid::Dto::Storage {

    struct CreateUploadResponse : BaseDto {

        /**
         * @brief Upload ID (UUID) identifying the multipart upload
         */
        std::string uploadId;

        /**
         * @brief ERN of the bucket the upload will be stored in
         */
        std::string bucketErn;

        /**
         * @brief Key (path) of the object being uploaded within the bucket
         */
        std::string key;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend CreateUploadResponse tag_invoke(boost::json::value_to_tag<CreateUploadResponse>, boost::json::value const &v) {
            CreateUploadResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.uploadId = Core::GetStringValue(v, "uploadId");
            r.bucketErn = Core::GetStringValue(v, "bucketErn");
            r.key = Core::GetStringValue(v, "key");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, CreateUploadResponse const &obj) {
            jv = {
                    {"uploadId", obj.uploadId},
                    {"bucketErn", obj.bucketErn},
                    {"key", obj.key},
            };
        }
    };
}// namespace Euclid::Dto::Storage
