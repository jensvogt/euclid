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

namespace Euclid::Dto::EAM {

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

    private:

        friend OidcAuthorizeResponse tag_invoke(boost::json::value_to_tag<OidcAuthorizeResponse>, boost::json::value const &v) {
            OidcAuthorizeResponse r;
            r.authorizationUrl = Core::GetStringValue(v, "authorizationUrl");
            r.state = Core::GetStringValue(v, "state");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, OidcAuthorizeResponse const &obj) {
            jv = {
                    {"authorizationUrl", obj.authorizationUrl},
                    {"state", obj.state},
            };
        }
    };

}
