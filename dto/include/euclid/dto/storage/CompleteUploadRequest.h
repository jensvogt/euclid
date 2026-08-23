//
// Created by vogje01 on 8/23/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::Storage {

    struct CompleteUploadRequest {

        /**
         * @brief Upload ID (UUID) returned by create-upload
         */
        std::string uploadId;

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static CompleteUploadRequest fromJson(const std::string &json) {
            return boost::json::value_to<CompleteUploadRequest>(Core::ParseJsonString(json));
        }

    private:

        friend CompleteUploadRequest tag_invoke(boost::json::value_to_tag<CompleteUploadRequest>, boost::json::value const &v) {
            CompleteUploadRequest r;
            r.uploadId = Core::GetStringValue(v, "uploadId");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, CompleteUploadRequest const &obj) {
            jv = {
                    {"uploadId", obj.uploadId},
            };
        }
    };
}// namespace Euclid::Dto::Storage
