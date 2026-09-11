// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/10/26.
//

#pragma once

// C++ includes
#include <optional>
#include <string>

// Euclid includes
#include <euclid/database/entity/ekv/Value.h>

namespace Euclid::Database::Entity::EKV {

    /**
     * @brief How a query narrows the items within one partition.
     *
     * @par
     * A query always names one partition exactly - that is what makes it a query rather than a
     * scan - and may then say which of that partition's items it wants by their sort key. Every
     * operator here is one the database can answer from the index, in order, which is the only
     * reason a sort key exists.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    struct SortCondition {

        enum class Operator {
            None,
            Equals,
            LessThan,
            LessOrEqual,
            GreaterThan,
            GreaterOrEqual,
            Between,
            BeginsWith
        };

        /**
         * @brief Which comparison to make. None takes the whole partition.
         */
        Operator op{Operator::None};

        /**
         * @brief What to compare against; the lower bound for Between.
         */
        Value value;

        /**
         * @brief The upper bound, for Between only. Inclusive, as the lower one is.
         */
        Value upper;

        /**
         * @brief Reads an operator from the word the API uses for it.
         *
         * @param name "eq", "lt", "le", "gt", "ge", "between" or "begins-with".
         * @return the operator, or std::nullopt if it is none of them.
         */
        static std::optional<Operator> OperatorFromString(const std::string &name);
    };

}// namespace Euclid::Database::Entity::EKV
