// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/2/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::ESM {

    using Json::tag_invoke;

    /**
     * @brief Names a bucket and the name it should have instead.
     *
     * @par
     * A bucket's name is part of its ERN, and an object's ERN carries the bucket's name too - so
     * renaming one is not an edit of a single field but a rewrite of everything that names it.
     * The bucket is addressed by ERN rather than by its current name, so a request cannot be
     * ambiguous about which bucket it means.
     */
    struct RenameBucketRequest {

        /**
         * @brief ERN of the bucket to rename
         */
        std::string ern;

        /**
         * @brief Name it should have
         */
        std::string newName;

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
        static RenameBucketRequest fromJson(const std::string &json) {
            return boost::json::value_to<RenameBucketRequest>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(RenameBucketRequest, (), (ern, newName))

}// namespace Euclid::Dto::ESM
