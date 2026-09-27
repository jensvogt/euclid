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

    struct DeleteBucketRequest {

        /**
         * @brief ERN of the bucket
         */
        std::string ern;

        /**
         * @brief Delete only while the bucket is empty; otherwise refuse.
         *
         * @par
         * delete-bucket takes the bucket's objects with it, by design - an object is only reachable
         * through its bucket, so rows left behind are unreachable for good. That makes it the right
         * behaviour for an operator who has decided, and the wrong one for anything automated: a
         * provisioning run that removes a line from a manifest must not be one mistake away from
         * destroying a bucket full of deliveries.
         *
         * @par
         * So a caller that only means "remove it if nothing is using it" says so, and gets a 409
         * naming what is still in there instead. Off by default: what "delete-bucket" has always
         * meant does not change.
         */
        bool ifEmpty{};

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
        static DeleteBucketRequest fromJson(const std::string &json) {
            return boost::json::value_to<DeleteBucketRequest>(Core::ParseJsonString(json));
        }

    private:

        friend DeleteBucketRequest tag_invoke(boost::json::value_to_tag<DeleteBucketRequest>, boost::json::value const &v) {
            DeleteBucketRequest r;
            r.ern = Core::GetStringValue(v, "ern");
            r.ifEmpty = Core::GetBoolValue(v, "ifEmpty");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, DeleteBucketRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"ifEmpty", obj.ifEmpty},
            };
        }
    };
}// namespace Euclid::Dto::ESM
