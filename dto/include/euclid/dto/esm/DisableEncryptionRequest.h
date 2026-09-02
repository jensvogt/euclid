//
// Created by vogje01 on 9/2/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ESM {

    /**
     * @brief Names the bucket whose objects should stop being encrypted at rest.
     *
     * @par
     * No key is named, because none is needed and none is touched: this says what happens to the
     * next object written to the bucket, and nothing about the objects already in it or the key
     * they are under.
     */
    struct DisableEncryptionRequest {

        /**
         * @brief ERN of the bucket to stop encrypting
         */
        std::string bucketErn;

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
        static DisableEncryptionRequest fromJson(const std::string &json) {
            return boost::json::value_to<DisableEncryptionRequest>(Core::ParseJsonString(json));
        }

    private:

        friend DisableEncryptionRequest tag_invoke(boost::json::value_to_tag<DisableEncryptionRequest>, boost::json::value const &v) {
            DisableEncryptionRequest r;
            r.bucketErn = Core::GetStringValue(v, "bucketErn");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, DisableEncryptionRequest const &obj) {
            jv = {
                    {"bucketErn", obj.bucketErn},
            };
        }
    };

}// namespace Euclid::Dto::ESM
