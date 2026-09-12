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
#include <euclid/core/DateTimeUtils.h>
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EAM {

    /**
     * @brief A named set of permissions, belonging to one account.
     *
     * @par
     * "builtin" tells a caller which roles it may change: the built-in roles are computed rather
     * than stored, and create/update/delete refuse them. Without it a UI has to know the six names.
     */
    struct Role {

        std::string name;
        std::string ern;
        std::string accountId;
        std::string region;
        std::string description;
        std::vector<std::string> permissions;
        bool builtin{false};
        std::string created;
        std::string modified;

        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend Role tag_invoke(boost::json::value_to_tag<Role>, boost::json::value const &v) {
            Role r;
            r.name = Core::GetStringValue(v, "name");
            r.ern = Core::GetStringValue(v, "ern");
            r.accountId = Core::GetStringValue(v, "accountId");
            r.region = Core::GetStringValue(v, "region");
            r.description = Core::GetStringValue(v, "description");
            for (const auto &permission: Core::GetStringArrayValue(v, "permissions")) r.permissions.push_back(permission);
            r.builtin = Core::GetBoolValue(v, "builtin");
            r.created = Core::GetStringValue(v, "created");
            r.modified = Core::GetStringValue(v, "modified");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, Role const &obj) {
            jv = {
                    {"name", obj.name},
                    {"ern", obj.ern},
                    {"accountId", obj.accountId},
                    {"region", obj.region},
                    {"description", obj.description},
                    {"permissions", boost::json::value_from(obj.permissions)},
                    {"builtin", obj.builtin},
                    {"created", obj.created},
                    {"modified", obj.modified},
            };
        }
    };

}
