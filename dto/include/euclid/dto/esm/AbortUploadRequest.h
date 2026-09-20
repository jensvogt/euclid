// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/20/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ESM {

    /**
     * @brief Throws away an upload that was started and will not be finished.
     *
     * @par
     * The counterpart complete-upload has always needed: a multipart upload that stops halfway
     * leaves its staged parts on disk under an id nothing will ever complete, and - for a first
     * upload - a row describing an object whose bytes do not exist. Neither goes away on its own,
     * because nothing else knows the difference between an upload that was abandoned and one that
     * is still being sent.
     */
    struct AbortUploadRequest {

        /**
         * @brief Upload ID (UUID) returned by create-upload
         */
        std::string uploadId;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static AbortUploadRequest fromJson(const std::string &json) {
            return boost::json::value_to<AbortUploadRequest>(Core::ParseJsonString(json));
        }

    private:

        friend AbortUploadRequest tag_invoke(boost::json::value_to_tag<AbortUploadRequest>, boost::json::value const &v) {
            AbortUploadRequest r;
            r.uploadId = Core::GetStringValue(v, "uploadId");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, AbortUploadRequest const &obj) {
            jv = {
                    {"uploadId", obj.uploadId},
            };
        }
    };
}// namespace Euclid::Dto::ESM
