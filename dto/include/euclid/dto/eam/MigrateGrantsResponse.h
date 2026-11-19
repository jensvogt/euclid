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
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::EAM {

    using Json::tag_invoke;

    /**
     * @brief What the migration wrote, or would have written.
     */
    struct MigrateGrantsResponse : BaseDto {

        /**
         * @brief Whether this was a rehearsal.
         */
        bool dryRun{true};

        /**
         * @brief One line per grant, e.g.
         * "user/alice -> operator in 000000000000 [production, staging]".
         */
        std::vector<std::string> grants;

        /**
         * @brief Grants that already existed and were left alone, so the migration can be re-run.
         */
        long skipped{};

        /**
         * @brief Users with nothing to migrate - no account grants and no resource grants.
         *
         * @par
         * The population shadow mode exists to size. Today they are unrestricted by virtue of
         * having no restrictions; under roles they have nothing at all, and what they actually
         * need has to be learned rather than guessed.
         */
        long unrestricted{};

        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(MigrateGrantsResponse, (BaseDto), (dryRun, grants, skipped, unrestricted))

}
