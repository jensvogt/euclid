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

namespace Euclid::Dto::EAM {

    /**
     * @brief Asks EAM to start an OIDC login and say where to send the browser.
     */
    struct OidcAuthorizeRequest {

        /**
         * @brief Where the provider should send the browser back to, or empty for the one
         * configured in oidc.redirect-uri.
         *
         * @par
         * The CLI names its own loopback listener here; a browser-driven flow leaves it empty and
         * gets the configured callback. Whatever is used is sealed into the state and presented
         * again when the code is redeemed, since the provider requires the two to match.
         */
        std::string redirectUri;

        /**
         * @brief Where to send the browser once login has succeeded, for a flow that ends in a
         * browser rather than in a caller collecting the response. Empty means answer with the
         * login response itself.
         */
        std::string returnTo;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend OidcAuthorizeRequest tag_invoke(boost::json::value_to_tag<OidcAuthorizeRequest>, boost::json::value const &v) {
            OidcAuthorizeRequest r;
            r.redirectUri = Core::GetStringValue(v, "redirectUri");
            r.returnTo = Core::GetStringValue(v, "returnTo");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, OidcAuthorizeRequest const &obj) {
            jv = {
                    {"redirectUri", obj.redirectUri},
                    {"returnTo", obj.returnTo},
            };
        }
    };

}
