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
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::EAM {

    using Json::tag_invoke;

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
    };

    BOOST_DESCRIBE_STRUCT(Grant, (), (grantId, role, principal, accountId, namespaces, resources, granted, grantedBy))

}
