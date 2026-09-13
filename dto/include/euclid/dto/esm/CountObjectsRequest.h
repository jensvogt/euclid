// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ESM {

    /**
     * @brief Asks how many objects a bucket holds, counted rather than looked up.
     *
     * @par
     * The difference from GetObjectCountRequest, which is worth knowing before choosing one: that
     * one answers the bucket's stored `objects` figure, which is a running total kept up to date
     * as objects come and go and recomputed periodically by the monitoring module. It costs one
     * document read whatever the bucket holds, and it is the whole bucket - there is nothing for a
     * prefix to narrow.
     *
     * @par
     * This one counts. It is exact at the moment it is asked, it can be narrowed to a prefix, and
     * it can be told to include the directory markers a listing hides. It costs a query over the
     * bucket's objects, so it is the one to reach for when the answer has to be right or has to be
     * about part of a bucket, and not the one to poll.
     */
    struct CountObjectsRequest {

        /**
         * @brief Bucket ERN
         */
        std::string ern{};

        /**
         * @brief Object key prefix; empty counts the whole bucket.
         */
        std::string prefix{};

        /**
         * @brief Whether to count the zero-byte markers that stand for directories.
         *
         * @par
         * Off by default, which matches what a listing shows and what the bucket's own `objects`
         * figure counts - a directory is not something a client stored.
         */
        bool includeDirectories{false};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend CountObjectsRequest tag_invoke(boost::json::value_to_tag<CountObjectsRequest>, boost::json::value const &v) {
            CountObjectsRequest r;
            r.ern = Core::GetStringValue(v, "ern");
            r.prefix = Core::GetStringValue(v, "prefix");
            r.includeDirectories = Core::GetBoolValue(v, "includeDirectories");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, CountObjectsRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"prefix", obj.prefix},
                    {"includeDirectories", obj.includeDirectories},
            };
        }
    };

}// namespace Euclid::Dto::ESM
