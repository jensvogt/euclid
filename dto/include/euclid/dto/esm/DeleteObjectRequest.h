// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/23/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ESM {

    struct DeleteObjectRequest {

        /**
         * @brief ERN of the object
         */
        std::string ern;

        /**
         * @brief Only this version of the object; empty deletes the object and every version of it.
         */
        std::string versionId;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]]
        std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]]
        static DeleteObjectRequest fromJson(const std::string &json) {
            return boost::json::value_to<DeleteObjectRequest>(Core::ParseJsonString(json));
        }

    private:

        friend DeleteObjectRequest tag_invoke(boost::json::value_to_tag<DeleteObjectRequest>, boost::json::value const &v) {
            DeleteObjectRequest r;
            r.ern = Core::GetStringValue(v, "ern");
            r.versionId = Core::GetStringValue(v, "versionId");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, DeleteObjectRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"versionId", obj.versionId},
            };
        }
    };
}// namespace Euclid::Dto::ESM