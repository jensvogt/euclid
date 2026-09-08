//
// Created by vogje01 on 9/8/26.
//

#pragma once

// Euclid includes
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/ess/model/Secret.h>

namespace Euclid::Dto::ESS {

    /**
     * @brief One secret with its value, as answered by get-secret and nothing else.
     */
    struct GetSecretResponse : BaseDto {

        /**
         * @brief The secret's metadata
         */
        Secret secret;

        /**
         * @brief The value, decrypted. The only place in euclid where one of these is written out.
         */
        std::string value;

        /**
         * @brief Serializes this response to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend GetSecretResponse tag_invoke(boost::json::value_to_tag<GetSecretResponse>, boost::json::value const &v) {
            GetSecretResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.secret = boost::json::value_to<Secret>(v.at("secret"));
            r.value = Core::GetStringValue(v, "value");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, GetSecretResponse const &obj) {
            jv = {
                    {"secret", boost::json::value_from(obj.secret)},
                    {"value", obj.value},
            };
        }
    };

}// namespace Euclid::Dto::ESS
