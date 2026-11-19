// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/12/26.
//

#pragma once

// C++ includes
#include <array>
#include <string>
#include <string_view>
#include <vector>

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::EAM {

    using Json::tag_invoke;

    /**
     * @brief Lists grants, by principal or by role.
     *
     * @par
     * The two questions this model exists to answer: "what may this caller do" and "who can do
     * this". Exactly one of the two fields is given.
     */
    struct ListGrantsRequest {

        /**
         * @brief A user or user-group ERN. When set, answers that principal's own grants - not its groups', which is a different question.
         */
        std::string principal;

        /**
         * @brief A role name. When set, answers every grant of that role in the account.
         */
        std::string role;

        /**
         * @brief Account to look in. The caller's own when empty.
         *
         * @par
         * Naming another needs administrator rights on it, the same as granting in one does - a
         * listing of who may do what in an account is not something to hand out more freely than
         * the ability to change it.
         */
        std::string accountId;

        /**
         * @brief How many grants to return. 0 or less is no limit, which is what a caller that
         * predates paging sends by sending nothing.
         */
        long pageSize{};

        /**
         * @brief Zero-based page, applied when pageSize is set.
         */
        long pageIndex{};

        /**
         * @brief Field to order by: principal, role, accountId or created.
         *
         * @par
         * Ordering matters more here than it does for a listing that is usually short: paging an
         * unordered collection can show the same grant on two pages and never show another, so
         * this defaults to a field rather than to none.
         */
        std::string sortColumn{"principal"};

        /**
         * @brief "asc" or "desc".
         */
        std::string sortDirection{"asc"};

        /**
         * @brief The members that keep their initial value when the JSON leaves them out, rather than
         * becoming zero, as the other members do. A caller that predates paging sends neither, and
         * an ordered answer is what makes its pages add up if it later starts asking for them.
         */
        static constexpr std::array<std::string_view, 2> kJsonDefaults{"sortColumn", "sortDirection"};

        /**
         * @brief Serializes this to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this from a JSON string
         */
        [[nodiscard]] static ListGrantsRequest fromJson(const std::string &json) {
            return boost::json::value_to<ListGrantsRequest>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(ListGrantsRequest, (), (principal, role, accountId, pageSize, pageIndex, sortColumn, sortDirection))

}
