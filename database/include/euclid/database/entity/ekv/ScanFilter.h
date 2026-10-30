// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <chrono>
#include <optional>
#include <string>

// Euclid includes
#include <euclid/database/entity/ekv/SortCondition.h>

namespace Euclid::Database::Entity::EKV {

    /**
     * @brief What to narrow a scan to, beyond the table it is over.
     *
     * @par
     * A query answers about one partition and is bounded by the index that partition is stored
     * under. This is the other question - "which items were touched since Friday", "which ones
     * have status OPEN" - which names no partition and so cannot be answered that way. It is still
     * a scan, with the narrowing done by the database rather than by whoever called it: the work
     * of reading the table is the same either way, and what changes is how much of it comes back
     * over the wire and how much a caller has to sift through.
     *
     * @par
     * Empty on every field is the whole table, which is what a scan has always been.
     */
    struct ScanFilter {

        /**
         * @brief Only items modified at or after this.
         */
        std::optional<std::chrono::system_clock::time_point> modifiedFrom;

        /**
         * @brief Only items modified at or before this.
         */
        std::optional<std::chrono::system_clock::time_point> modifiedTo;

        /**
         * @brief The attribute to compare, or empty for none. Any attribute of the item, not only
         * a key one - which is the point: a key is what a query already reaches.
         */
        std::string attributeName;

        /**
         * @brief How to compare it, in the same vocabulary a query uses for the sort key. None
         * means "the attribute is not being compared", so naming an attribute with no operator
         * narrows nothing.
         */
        SortCondition attribute;

        /**
         * @brief Whether anything is being narrowed at all.
         */
        [[nodiscard]] bool empty() const {
            return !modifiedFrom.has_value() && !modifiedTo.has_value()
                   && (attributeName.empty() || attribute.op == SortCondition::Operator::None);
        }
    };

}// namespace Euclid::Database::Entity::EKV
