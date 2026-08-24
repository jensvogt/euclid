//
// Created by vogje01 on 8/18/26.
//

#pragma once

// C++ includes
#include <string>

// Boost includes
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EAM {

    struct DeleteAccessKeyRequest {

        /**
         * @brief Access key ID to delete, e.g. "AKIA...".
         */
        std::string accessKeyId;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend DeleteAccessKeyRequest tag_invoke(boost::json::value_to_tag<DeleteAccessKeyRequest>, boost::json::value const &v) {
            DeleteAccessKeyRequest r;
            r.accessKeyId = Core::GetStringValue(v, "accessKeyId");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, DeleteAccessKeyRequest const &obj) {
            jv = {
                    {"accessKeyId", obj.accessKeyId},
            };
        }
    };

}
