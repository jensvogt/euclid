// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/8/26.
//

#pragma once

// Euclid includes
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/ess/model/Secret.h>

namespace Euclid::Dto::ESS {

    /**
     * @brief One secret's metadata, as answered by create-secret and update-secret.
     *
     * @par
     * Without the value, deliberately: a caller that has just written one already has it, and an
     * answer that echoed it would put it into a log, a shell history and a terminal for no reason.
     */
    struct SecretResponse : BaseDto {

        /**
         * @brief The stored secret, without its value
         */
        Secret secret;

        /**
         * @brief Serializes this response to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend SecretResponse tag_invoke(boost::json::value_to_tag<SecretResponse>, boost::json::value const &v) {
            SecretResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.secret = boost::json::value_to<Secret>(v.at("secret"));
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, SecretResponse const &obj) {
            jv = {{"secret", boost::json::value_from(obj.secret)}};
        }
    };

}// namespace Euclid::Dto::ESS
