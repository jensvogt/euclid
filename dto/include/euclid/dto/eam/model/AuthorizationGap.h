// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/12/26.
//

#pragma once

// C++ includes
#include <string>

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EAM {

    /**
     * @brief Something shadow mode would have refused, and how often.
     */
    struct AuthorizationGap {

        std::string userId;
        std::string accountId;
        std::string nameSpace;
        std::string target;
        std::string action;
        std::string reason;
        long count{};
        std::string firstSeen;
        std::string lastSeen;

        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend AuthorizationGap tag_invoke(boost::json::value_to_tag<AuthorizationGap>, boost::json::value const &v) {
            AuthorizationGap g;
            g.userId = Core::GetStringValue(v, "userId");
            g.accountId = Core::GetStringValue(v, "accountId");
            g.nameSpace = Core::GetStringValue(v, "namespace");
            g.target = Core::GetStringValue(v, "target");
            g.action = Core::GetStringValue(v, "action");
            g.reason = Core::GetStringValue(v, "reason");
            g.count = Core::GetLongValue(v, "count");
            g.firstSeen = Core::GetStringValue(v, "firstSeen");
            g.lastSeen = Core::GetStringValue(v, "lastSeen");
            return g;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, AuthorizationGap const &obj) {
            jv = {
                    {"userId", obj.userId},
                    {"accountId", obj.accountId},
                    {"namespace", obj.nameSpace},
                    {"target", obj.target},
                    {"action", obj.action},
                    {"reason", obj.reason},
                    {"count", obj.count},
                    {"firstSeen", obj.firstSeen},
                    {"lastSeen", obj.lastSeen},
            };
        }
    };

}
