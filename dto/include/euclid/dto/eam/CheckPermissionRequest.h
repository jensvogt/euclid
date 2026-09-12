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
     * @brief Asks whether a user would be allowed to do something.
     *
     * @par
     * The first question anybody asks of a permission system is "why can they" or "why can't
     * they", and one that cannot answer gets worked around by making everybody an administrator.
     * This answers with the verdict, the reason, and the role that decided it.
     */
    struct CheckPermissionRequest {

        /**
         * @brief The user to ask about. Their groups' grants count too, the same way they would on a real request.
         */
        std::string userId;

        /**
         * @brief Module, e.g. "ens".
         */
        std::string target;

        /**
         * @brief Action, e.g. "publish-message".
         */
        std::string action;

        /**
         * @brief Namespace to ask about; empty is the account root, which is a scope like any other.
         */
        std::string nameSpace;

        /**
         * @brief The resource, for the actions that name one. Empty otherwise.
         */
        std::string resourceErn;

        /**
         * @brief Serializes this to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this from a JSON string
         */
        [[nodiscard]] static CheckPermissionRequest fromJson(const std::string &json) {
            return boost::json::value_to<CheckPermissionRequest>(Core::ParseJsonString(json));
        }

    private:

        friend CheckPermissionRequest tag_invoke(boost::json::value_to_tag<CheckPermissionRequest>, boost::json::value const &v) {
            CheckPermissionRequest r;
            r.userId = Core::GetStringValue(v, "userId");
            r.target = Core::GetStringValue(v, "target");
            r.action = Core::GetStringValue(v, "action");
            r.nameSpace = Core::GetStringValue(v, "namespace");
            r.resourceErn = Core::GetStringValue(v, "resourceErn");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, CheckPermissionRequest const &obj) {
            jv = {
                    {"userId", obj.userId},
                    {"target", obj.target},
                    {"action", obj.action},
                    {"namespace", obj.nameSpace},
                    {"resourceErn", obj.resourceErn},
            };
        }
    };

}
