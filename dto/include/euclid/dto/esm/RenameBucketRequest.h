//
// Created by vogje01 on 9/2/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ESM {

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

    private:

        friend RenameBucketRequest tag_invoke(boost::json::value_to_tag<RenameBucketRequest>, boost::json::value const &v) {
            RenameBucketRequest r;
            r.ern = Core::GetStringValue(v, "ern");
            r.newName = Core::GetStringValue(v, "newName");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, RenameBucketRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"newName", obj.newName},
            };
        }
    };

}// namespace Euclid::Dto::ESM
