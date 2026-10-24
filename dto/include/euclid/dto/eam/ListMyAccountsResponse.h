// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <string>
#include <vector>

// Euclid includes
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/eam/model/Account.h>

namespace Euclid::Dto::EAM {

    /**
     * @brief The accounts the caller may work in.
     *
     * @par
     * Not ListAccountsResponse with a different filter: that one answers "every account there is"
     * and is an administrator's view of the installation. This one is about the caller, which is
     * why it needs a field that one has no use for - a client offering a choice has to know which
     * of them to offer first, and the home account is the answer whenever nothing else is known.
     */
    struct ListMyAccountsResponse : BaseDto {

        /**
         * @brief The accounts, by account id.
         */
        std::vector<Account> accounts;

        /**
         * @brief total number of accounts
         */
        long total{};

        /**
         * @brief The caller's own account - the one they were registered in.
         *
         * @par
         * Always among @ref accounts, and the one a client should select when the user has not
         * said otherwise: it is where their own things are, and it is the account every session
         * before this field existed worked in.
         */
        std::string homeAccountId;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend ListMyAccountsResponse tag_invoke(boost::json::value_to_tag<ListMyAccountsResponse>, boost::json::value const &v) {
            ListMyAccountsResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.accounts = boost::json::value_to<std::vector<Account> >(v.at("accounts"));
            r.total = Core::GetLongValue(v, "total");
            r.homeAccountId = Core::GetStringValue(v, "homeAccountId");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ListMyAccountsResponse const &obj) {
            jv = {
                    {"accounts", boost::json::value_from(obj.accounts)},
                    {"total", obj.total},
                    {"homeAccountId", obj.homeAccountId},
            };
        }
    };

}
