// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <string>

// Boost includes
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EAM {

    struct ChangePasswordRequest {

        /**
         * @brief User whose password is being changed, or empty for the caller's own.
         *
         * Naming somebody else is an administrator's reset and is refused to anybody else;
         * naming yourself is the same request as leaving it empty.
         */
        std::string userId;

        /**
         * @brief The password being replaced (plaintext), which is what proves a caller may
         * replace it.
         *
         * Required when changing your own, and not used at all for an administrator's reset -
         * that one is proved by being an administrator.
         */
        std::string oldPassword;

        /**
         * @brief The new password (plaintext, hashed before storage)
         */
        std::string newPassword;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend ChangePasswordRequest tag_invoke(boost::json::value_to_tag<ChangePasswordRequest>, boost::json::value const &v) {
            ChangePasswordRequest r;
            r.userId = Core::GetStringValue(v, "userId");
            r.oldPassword = Core::GetStringValue(v, "oldPassword");
            r.newPassword = Core::GetStringValue(v, "newPassword");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ChangePasswordRequest const &obj) {
            jv = {
                    {"userId", obj.userId},
                    {"oldPassword", obj.oldPassword},
                    {"newPassword", obj.newPassword},
            };
        }
    };

}
