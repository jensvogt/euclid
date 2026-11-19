// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <array>
#include <string>
#include <string_view>
#include <vector>

// Euclid includes
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/eam/model/Account.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::EAM {

    using Json::tag_invoke;

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
         * @brief The members that must be in the JSON: missing or null, reading throws.
         */
        static constexpr std::array<std::string_view, 1> kJsonRequired{"accounts"};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(ListMyAccountsResponse, (BaseDto), (accounts, total, homeAccountId))

}
