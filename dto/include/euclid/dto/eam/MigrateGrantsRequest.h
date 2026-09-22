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
     * @brief Turns the old per-user grants into role bindings.
     */
    struct MigrateGrantsRequest {

        /**
         * @brief Report what would be written without writing it.
         *
         * @par
         * On by default. This reads every user in the installation and creates grants for them,
         * and an operator should see the list before it happens rather than after.
         */
        bool dryRun = true;

        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        [[nodiscard]] static MigrateGrantsRequest fromJson(const std::string &json) {
            return boost::json::value_to<MigrateGrantsRequest>(Core::ParseJsonString(json));
        }

    private:

        friend MigrateGrantsRequest tag_invoke(boost::json::value_to_tag<MigrateGrantsRequest>, boost::json::value const &v) {
            MigrateGrantsRequest r;
            r.dryRun = Core::AttributeExists(v, "dryRun") ? Core::GetBoolValue(v, "dryRun") : true;
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, MigrateGrantsRequest const &obj) {
            jv = {
                    {"dryRun", obj.dryRun},
            };
        }
    };

}
