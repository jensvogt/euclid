//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <string>

// Boost includes
#include <boost/json/conversion.hpp>

// Euclid include
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>

namespace Euclid::Dto::EAM {

    struct LoginResponse : BaseDto {

        /**
         * @brief JWT token
         */
        std::string token;

        /**
         * @brief Public identifier of the caller's SigV4 access key, e.g. "AKIA...". Reused from
         * an existing active key if the user already has one, otherwise provisioned on this login.
         */
        std::string accessKeyId;

        /**
         * @brief Secret used to sign requests, paired with accessKeyId. May be a value already
         * returned on a prior login (not necessarily freshly generated), since login reuses an
         * existing active key rather than minting a new one each time.
         */
        std::string secretAccessKey;

        /**
         * @brief Creation timestamp of the access key, ISO8601 (not the login time).
         */
        std::string createdAt;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend LoginResponse tag_invoke(boost::json::value_to_tag<LoginResponse>, boost::json::value const &v) {
            LoginResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.token = Core::GetStringValue(v, "token");
            r.accessKeyId = Core::GetStringValue(v, "accessKeyId");
            r.secretAccessKey = Core::GetStringValue(v, "secretAccessKey");
            r.createdAt = Core::GetStringValue(v, "createdAt");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, LoginResponse const &obj) {
            jv = {
                    {"metadata", boost::json::value_from(static_cast<const BaseDto &>(obj))},
                    {"token", obj.token},
                    {"accessKeyId", obj.accessKeyId},
                    {"secretAccessKey", obj.secretAccessKey},
                    {"createdAt", obj.createdAt},
            };
        }
    };

}