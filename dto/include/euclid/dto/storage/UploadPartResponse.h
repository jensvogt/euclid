//
// Created by vogje01 on 8/23/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>

namespace Euclid::Dto::Storage {

    struct UploadPartResponse : BaseDto {

        /**
         * @brief Upload ID (UUID) this part belongs to
         */
        std::string uploadId;

        /**
         * @brief 1-based part number
         */
        long partNumber = 0;

        /**
         * @brief Size of the stored part, in bytes
         */
        long size = 0;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend UploadPartResponse tag_invoke(boost::json::value_to_tag<UploadPartResponse>, boost::json::value const &v) {
            UploadPartResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.uploadId = Core::GetStringValue(v, "uploadId");
            r.partNumber = Core::GetLongValue(v, "partNumber");
            r.size = Core::GetLongValue(v, "size");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, UploadPartResponse const &obj) {
            jv = {
                    {"uploadId", obj.uploadId},
                    {"partNumber", obj.partNumber},
                    {"size", obj.size},
            };
        }
    };
}// namespace Euclid::Dto::Storage
