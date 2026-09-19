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
     * @brief Reads one user group, by name or by ERN.
     *
     * @par
     * Groups are installation-wide rather than scoped to an account, so a name identifies one
     * without further qualification - which is why a name is enough here and an ERN is offered
     * only because that is what a grant's principal carries.
     */
    struct GetUserGroupRequest {

        /**
         * @brief Group ERN, or empty when the group is named instead.
         */
        std::string ern;

        /**
         * @brief Group name, used when no ERN is given.
         */
        std::string name;

        /**
         * @brief Serializes this to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend GetUserGroupRequest tag_invoke(boost::json::value_to_tag<GetUserGroupRequest>, boost::json::value const &v) {
            GetUserGroupRequest r;
            r.ern = Core::GetStringValue(v, "ern");
            r.name = Core::GetStringValue(v, "name");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, GetUserGroupRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"name", obj.name},
            };
        }
    };
}// namespace Euclid::Dto::EAM
