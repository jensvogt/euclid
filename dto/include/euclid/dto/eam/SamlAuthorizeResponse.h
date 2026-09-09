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
     * @brief Where to send the person to authenticate with the SAML identity provider.
     */
    struct SamlAuthorizeResponse {

        /**
         * @brief The provider's single sign-on URL, with the deflated, encoded AuthnRequest and the
         * RelayState already on it. Open it in a browser; nothing else needs to be added.
         */
        std::string authenticationUrl;

        /**
         * @brief The opaque RelayState this attempt travels under, also present in
         * authenticationUrl.
         *
         * @par
         * Returned separately so a caller can tell whether what comes back belongs to the login it
         * started. It carries the request ID the assertion has to answer, sealed - there is
         * nothing in it to read.
         */
        std::string relayState;

        /**
         * @brief Serializes this response to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend SamlAuthorizeResponse tag_invoke(boost::json::value_to_tag<SamlAuthorizeResponse>, boost::json::value const &v) {
            SamlAuthorizeResponse r;
            r.authenticationUrl = Core::GetStringValue(v, "authenticationUrl");
            r.relayState = Core::GetStringValue(v, "relayState");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, SamlAuthorizeResponse const &obj) {
            jv = {
                    {"authenticationUrl", obj.authenticationUrl},
                    {"relayState", obj.relayState},
            };
        }
    };

}
