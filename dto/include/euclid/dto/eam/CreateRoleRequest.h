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
     * @brief Creates a role in the caller's own account.
     *
     * @par
     * The account is the caller's, taken from the session rather than named here - a role is
     * created where the person creating it works, and naming another account would be a way to
     * write into one they may not.
     */
    struct CreateRoleRequest {

        /**
         * @brief Role name, unique within the account. May not be one of the built-in names.
         */
        std::string name;

        /**
         * @brief Free-text description of what the role is for.
         */
        std::string description;

        /**
         * @brief What the role grants: "<module>:<action>", "<module>:*" or "*:*". Every entry is checked against the vocabulary, so a permission no module dispatches is refused rather than stored.
         */
        std::vector<std::string> permissions;

        /**
         * @brief Serializes this to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this from a JSON string
         */
        [[nodiscard]] static CreateRoleRequest fromJson(const std::string &json) {
            return boost::json::value_to<CreateRoleRequest>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(CreateRoleRequest, (), (name, description, permissions))

}
