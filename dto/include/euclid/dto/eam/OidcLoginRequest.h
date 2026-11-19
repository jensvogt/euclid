// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/9/26.
//

#pragma once

// C++ includes
#include <string>

// Boost includes
#include <boost/json/value.hpp>

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::EAM {

    using Json::tag_invoke;

    /**
     * @brief Hands EAM what came back from the provider, to be turned into a euclid session.
     *
     * @par
     * Answered with the same Dto::EAM::LoginResponse a password login produces - the token, the
     * access key and the admin flag - because from here on a federated session is not a different
     * kind of session.
     */
    struct OidcLoginRequest {

        /**
         * @brief The authorization code from the provider's callback.
         */
        std::string code;

        /**
         * @brief The state from the provider's callback, exactly as it arrived.
         *
         * @par
         * Not merely checked against the one that was issued: it *is* the flow's memory - the
         * nonce and the PKCE verifier are sealed inside it - so a callback without it cannot be
         * completed by any instance, and one that has been altered is refused.
         */
        std::string state;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(OidcLoginRequest, (), (code, state))

}
