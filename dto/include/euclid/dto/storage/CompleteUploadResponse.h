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
         * @brief Total size of the assembled object, in bytes
         */
        long size = 0;

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
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, CompleteUploadResponse const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"bucketErn", obj.bucketErn},
                    {"key", obj.key},
                    {"size", obj.size},
            };
        }
    };
}// namespace Euclid::Dto::Storage
