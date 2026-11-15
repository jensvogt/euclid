// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/1/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::ESM {

    using Json::tag_invoke;

    /**
     * @brief Names an object and the key it should have instead.
     *
     * @par
     * A move within one bucket, said the way an operator means it. The bucket is named once,
     * because renaming into a different bucket is not a rename - that is move-object.
     */
    struct RenameObjectRequest {

        /**
         * @brief ERN of the bucket the object is in
         */
        std::string bucketErn;

        /**
         * @brief Current key of the object
         */
        std::string key;

        /**
         * @brief Key it should have
         */
        std::string newKey;

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
        static RenameObjectRequest fromJson(const std::string &json) {
            return boost::json::value_to<RenameObjectRequest>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(RenameObjectRequest, (), (bucketErn, key, newKey))

}// namespace Euclid::Dto::ESM
