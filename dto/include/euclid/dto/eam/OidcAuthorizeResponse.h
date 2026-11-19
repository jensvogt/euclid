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
     * @brief Where to send the person to authenticate, and the state that belongs to that
     * particular attempt.
     */
    struct OidcAuthorizeResponse {

        /**
         * @brief The provider's authorization endpoint, with every parameter already on it. Open
         * it in a browser; nothing else needs to be added.
         */
        std::string authorizationUrl;

        /**
         * @brief The opaque state this attempt travels under, also present in authorizationUrl.
         *
         * @par
         * Returned separately so a caller can check that the callback it receives belongs to the
         * authorization it started. It carries the flow's nonce and PKCE verifier, sealed - there
         * is nothing in it to read, and nothing a caller has to keep secret beyond not handing it
         * to somebody else.
         */
        std::string state;

        /**
         * @brief Serializes this response to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(OidcAuthorizeResponse, (), (authorizationUrl, state))

}
