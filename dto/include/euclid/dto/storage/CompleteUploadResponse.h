//
// Created by vogje01 on 8/23/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>

namespace Euclid::Dto::Storage {

    struct CompleteUploadResponse : BaseDto {

        /**
         * @brief Euclid resource name of the completed object
         */
        std::string ern;

        /**
         * @brief ERN of the bucket the object was stored in
         */
        std::string bucketErn;

        /**
         * @brief Key (path) of the object within the bucket
         */
        std::string key;

        /**
         * @brief Size of the upload's parts combined, in bytes
         */
        long size = 0;

        /**
         * @brief Object lifecycle status as of this response - always "UPLOADED" here, since
         * complete-upload returns as soon as all parts are confirmed present, before the
         * background post-processing (assembly, content-type detection, MD5) that sets the
         * object to "COMPLETED" has run.
         */
        std::string status;

        /**
         * @brief MIME content type, determined during post-processing; empty in this response,
         * since post-processing runs asynchronously after complete-upload returns
         */
        std::string contentType;

        /**
         * @brief MD5 checksum of the assembled object, hex-encoded, computed during post-processing;
         * empty in this response, since post-processing runs asynchronously after complete-upload returns
         */
        std::string md5Sum;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend CompleteUploadResponse tag_invoke(boost::json::value_to_tag<CompleteUploadResponse>, boost::json::value const &v) {
            CompleteUploadResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.ern = Core::GetStringValue(v, "ern");
            r.bucketErn = Core::GetStringValue(v, "bucketErn");
            r.key = Core::GetStringValue(v, "key");
            r.size = Core::GetLongValue(v, "size");
            r.status = Core::GetStringValue(v, "status");
            r.contentType = Core::GetStringValue(v, "contentType");
            r.md5Sum = Core::GetStringValue(v, "md5Sum");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, CompleteUploadResponse const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"bucketErn", obj.bucketErn},
                    {"key", obj.key},
                    {"size", obj.size},
                    {"status", obj.status},
                    {"contentType", obj.contentType},
                    {"md5Sum", obj.md5Sum},
            };
        }
    };
}// namespace Euclid::Dto::Storage
