// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/12/26.
//

#pragma once

// C++ includes
#include <array>
#include <string>
#include <string_view>

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::EAM {

    using Json::tag_invoke;

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

        /**
         * @brief The members that keep their initial value when the JSON leaves them out, rather than
         * becoming zero, as the other members do. A request that leaves it out stays a dry run.
         */
        static constexpr std::array<std::string_view, 1> kJsonDefaults{"dryRun"};

        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        [[nodiscard]] static MigrateGrantsRequest fromJson(const std::string &json) {
            return boost::json::value_to<MigrateGrantsRequest>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(MigrateGrantsRequest, (), (dryRun))

}
