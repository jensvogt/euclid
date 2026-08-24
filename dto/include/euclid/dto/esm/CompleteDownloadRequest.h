//
// Created by vogje01 on 8/24/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ESM {

    struct CompleteDownloadRequest {

        /**
         * @brief Download ID (UUID) returned by create-download
         */
        std::string downloadId;

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static CompleteDownloadRequest fromJson(const std::string &json) {
            return boost::json::value_to<CompleteDownloadRequest>(Core::ParseJsonString(json));
        }

    private:

        friend CompleteDownloadRequest tag_invoke(boost::json::value_to_tag<CompleteDownloadRequest>, boost::json::value const &v) {
            CompleteDownloadRequest r;
            r.downloadId = Core::GetStringValue(v, "downloadId");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, CompleteDownloadRequest const &obj) {
            jv = {
                    {"downloadId", obj.downloadId},
            };
        }
    };
}// namespace Euclid::Dto::ESM
