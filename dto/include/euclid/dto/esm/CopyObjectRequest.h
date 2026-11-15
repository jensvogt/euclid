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
     * @brief Names an object and where it should end up.
     *
     * @par
     * Shared by copy-object and move-object, which take the same four fields and differ only in
     * what happens to the source afterwards - one leaves it alone, the other stops referring to
     * it. Renaming has its own request, because naming the same bucket twice to change a key is
     * a worse way to say it.
     */
    struct CopyObjectRequest {

        /**
         * @brief ERN of the bucket the object is in
         */
        std::string sourceBucketErn;

        /**
         * @brief Key of the object within that bucket
         */
        std::string sourceKey;

        /**
         * @brief ERN of the bucket the object should end up in; may be the source bucket
         */
        std::string targetBucketErn;

        /**
         * @brief Key the object should have there
         */
        std::string targetKey;

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
        static CopyObjectRequest fromJson(const std::string &json) {
            return boost::json::value_to<CopyObjectRequest>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(CopyObjectRequest, (), (sourceBucketErn, sourceKey, targetBucketErn, targetKey))

}// namespace Euclid::Dto::ESM
