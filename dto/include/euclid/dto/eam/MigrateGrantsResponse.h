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

namespace Euclid::Dto::EAM {

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

    private:

        friend MigrateGrantsResponse tag_invoke(boost::json::value_to_tag<MigrateGrantsResponse>, boost::json::value const &v) {
            MigrateGrantsResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.dryRun = Core::GetBoolValue(v, "dryRun");
            for (const auto &grant: Core::GetStringArrayValue(v, "grants")) r.grants.push_back(grant);
            r.skipped = Core::GetLongValue(v, "skipped");
            r.unrestricted = Core::GetLongValue(v, "unrestricted");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, MigrateGrantsResponse const &obj) {
            jv = {
                    {"metadata", boost::json::value_from(static_cast<const BaseDto &>(obj))},
                    {"dryRun", obj.dryRun},
                    {"grants", boost::json::value_from(obj.grants)},
                    {"skipped", obj.skipped},
                    {"unrestricted", obj.unrestricted},
            };
        }
    };

}
