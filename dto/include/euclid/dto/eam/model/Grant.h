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
     * @brief One role, given to one principal, somewhere.
     *
     * @par
     * "grantId" is what revoke-role takes. It is the grant's own id rather than the (role,
     * principal) pair, because the same role may be granted to the same principal twice with
     * different scope and revoking has to say which.
     */
    struct Grant {

        std::string grantId;
        std::string role;
        std::string principal;
        std::string accountId;
        std::vector<std::string> namespaces;
        std::vector<std::string> resources;
        std::string granted;
        std::string grantedBy;

        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend Grant tag_invoke(boost::json::value_to_tag<Grant>, boost::json::value const &v) {
            Grant g;
            g.grantId = Core::GetStringValue(v, "grantId");
            g.role = Core::GetStringValue(v, "role");
            g.principal = Core::GetStringValue(v, "principal");
            g.accountId = Core::GetStringValue(v, "accountId");
            for (const auto &ns: Core::GetStringArrayValue(v, "namespaces")) g.namespaces.push_back(ns);
            for (const auto &resource: Core::GetStringArrayValue(v, "resources")) g.resources.push_back(resource);
            g.granted = Core::GetStringValue(v, "granted");
            g.grantedBy = Core::GetStringValue(v, "grantedBy");
            return g;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, Grant const &obj) {
            jv = {
                    {"grantId", obj.grantId},
                    {"role", obj.role},
                    {"principal", obj.principal},
                    {"accountId", obj.accountId},
                    {"namespaces", boost::json::value_from(obj.namespaces)},
                    {"resources", boost::json::value_from(obj.resources)},
                    {"granted", obj.granted},
                    {"grantedBy", obj.grantedBy},
            };
        }
    };

}
