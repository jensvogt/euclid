//
// Created by vogje01 on 9/1/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ESM {

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

    private:

        friend CopyObjectRequest tag_invoke(boost::json::value_to_tag<CopyObjectRequest>, boost::json::value const &v) {
            CopyObjectRequest r;
            r.sourceBucketErn = Core::GetStringValue(v, "sourceBucketErn");
            r.sourceKey = Core::GetStringValue(v, "sourceKey");
            r.targetBucketErn = Core::GetStringValue(v, "targetBucketErn");
            r.targetKey = Core::GetStringValue(v, "targetKey");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, CopyObjectRequest const &obj) {
            jv = {
                    {"sourceBucketErn", obj.sourceBucketErn},
                    {"sourceKey", obj.sourceKey},
                    {"targetBucketErn", obj.targetBucketErn},
                    {"targetKey", obj.targetKey},
            };
        }
    };

}// namespace Euclid::Dto::ESM
