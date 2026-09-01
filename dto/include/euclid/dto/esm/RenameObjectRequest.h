//
// Created by vogje01 on 9/1/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ESM {

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

    private:

        friend RenameObjectRequest tag_invoke(boost::json::value_to_tag<RenameObjectRequest>, boost::json::value const &v) {
            RenameObjectRequest r;
            r.bucketErn = Core::GetStringValue(v, "bucketErn");
            r.key = Core::GetStringValue(v, "key");
            r.newKey = Core::GetStringValue(v, "newKey");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, RenameObjectRequest const &obj) {
            jv = {
                    {"bucketErn", obj.bucketErn},
                    {"key", obj.key},
                    {"newKey", obj.newKey},
            };
        }
    };

}// namespace Euclid::Dto::ESM
