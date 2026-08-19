//
// Created by vogje01 on 8/18/26.
//

#pragma once

// C++ includes
#include <string>

// Euclid includes
#include <euclid/dto/BaseDto.h>

namespace Euclid::Dto::Access {

    struct CreateAccessKeyResponse : BaseDto {

        /**
         * @brief Public identifier of the newly created key, e.g. "AKIA...".
         */
        std::string accessKeyId;

        /**
         * @brief Secret used to sign requests. Shown only this once via this endpoint - not
         * retrievable again here, though "access login" will also return it if this key is later
         * reused as the caller's active key.
         */
        std::string secretAccessKey;

        /**
         * @brief Creation timestamp, ISO8601.
         */
        std::string createdAt;

        /**
         * @brief Serializes this response to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend CreateAccessKeyResponse tag_invoke(boost::json::value_to_tag<CreateAccessKeyResponse>, boost::json::value const &v) {
            CreateAccessKeyResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.accessKeyId = Core::GetStringValue(v, "accessKeyId");
            r.secretAccessKey = Core::GetStringValue(v, "secretAccessKey");
            r.createdAt = Core::GetStringValue(v, "createdAt");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, CreateAccessKeyResponse const &obj) {
            jv = {
                    {"metadata", boost::json::value_from(static_cast<const BaseDto &>(obj))},
                    {"accessKeyId", obj.accessKeyId},
                    {"secretAccessKey", obj.secretAccessKey},
                    {"createdAt", obj.createdAt},
            };
        }
    };

}
