// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/19/26.
//

#pragma once

// C++ includes
#include <string>

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EAM {

    /**
     * @brief Asks for one account, by account ID or by ERN.
     *
     * @par
     * The account ID rather than a name, because that is what everything else names an account
     * with: an ERN's fourth field, a grant's scope, every resource's owner. The name is
     * descriptive and is not what anything is addressed by, which is why it is not a way in here.
     */
    struct GetAccountRequest {

        /**
         * @brief Account ID, or empty when the account is named by ERN instead.
         */
        std::string accountId{};

        /**
         * @brief Account ERN, used when no account ID is given.
         */
        std::string ern{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend GetAccountRequest tag_invoke(boost::json::value_to_tag<GetAccountRequest>, boost::json::value const &v) {
            GetAccountRequest r;
            r.accountId = Core::GetStringValue(v, "accountId");
            r.ern = Core::GetStringValue(v, "ern");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, GetAccountRequest const &obj) {
            jv = {
                    {"accountId", obj.accountId},
                    {"ern", obj.ern},
            };
        }
    };
}// namespace Euclid::Dto::EAM
