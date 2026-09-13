// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/12/26.
//

#pragma once

// C++ includes
#include <string>
#include <vector>

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EAM {

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

    private:

        friend ListGrantsRequest tag_invoke(boost::json::value_to_tag<ListGrantsRequest>, boost::json::value const &v) {
            ListGrantsRequest r;
            r.principal = Core::GetStringValue(v, "principal");
            r.role = Core::GetStringValue(v, "role");
            r.accountId = Core::GetStringValue(v, "accountId");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ListGrantsRequest const &obj) {
            jv = {
                    {"principal", obj.principal},
                    {"role", obj.role},
                    {"accountId", obj.accountId},
            };
        }
    };

}
