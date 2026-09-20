// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/20/26.
//

#pragma once

// C++ includes
#include <string>

// Euclid includes
#include <euclid/dto/BaseDto.h>

namespace Euclid::Dto::ESM {

    /**
     * @brief What an abandoned upload was, and what became of the object it was writing.
     */
    struct AbortUploadResponse : BaseDto {

        /**
         * @brief The upload that was discarded.
         */
        std::string uploadId;

        /**
         * @brief Bucket it was being written to.
         */
        std::string bucketErn;

        /**
         * @brief Key it was being written to.
         */
        std::string key;

        /**
         * @brief How many staged parts were thrown away.
         */
        long parts{};

        /**
         * @brief Whether the object row at that key was removed with the upload.
         *
         * @par
         * True for a first upload, whose row described bytes that never arrived. False for a
         * re-upload, where the row is the previous version - still published, still readable, and
         * not this upload's to delete. Reported rather than left to be inferred, because "the
         * upload is gone" and "the object is gone" are different answers and a caller cleaning up
         * after a failure needs to know which one they got.
         */
        bool objectRemoved{};

        /**
         * @brief Serializes this response to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend AbortUploadResponse tag_invoke(boost::json::value_to_tag<AbortUploadResponse>, boost::json::value const &v) {
            AbortUploadResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.uploadId = Core::GetStringValue(v, "uploadId");
            r.bucketErn = Core::GetStringValue(v, "bucketErn");
            r.key = Core::GetStringValue(v, "key");
            r.parts = Core::GetLongValue(v, "parts");
            r.objectRemoved = Core::GetBoolValue(v, "objectRemoved");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, AbortUploadResponse const &obj) {
            jv = {
                    {"uploadId", obj.uploadId},
                    {"bucketErn", obj.bucketErn},
                    {"key", obj.key},
                    {"parts", obj.parts},
                    {"objectRemoved", obj.objectRemoved},
            };
        }
    };
}// namespace Euclid::Dto::ESM
