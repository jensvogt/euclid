// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/8/26.
//

#pragma once

// C++ includes
#include <optional>
#include <string>
#include <vector>

// MongoDB includes
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/oid.hpp>

namespace Euclid::Database::Emd {

    /**
     * @brief What an update did, in the terms the driver reports it.
     */
    struct UpdateResult {

        /**
         * @brief How many documents the filter selected.
         */
        long matched{};

        /**
         * @brief How many were actually changed. Fewer than @ref matched when an update sets a
         * field to what it already held.
         */
        long modified{};

        /**
         * @brief The id of the document an upsert created, or nothing when it matched an existing
         * one.
         */
        std::optional<bsoncxx::oid> upsertedId;
    };

    /**
     * @brief How a find is narrowed and ordered.
     */
    struct FindOptions {

        /**
         * @brief Sort specification, e.g. { "name": 1, "created": -1 }. Empty leaves the order
         * unspecified, which here means insertion order.
         */
        std::optional<bsoncxx::document::value> sort;

        /**
         * @brief Maximum number of documents to return; 0 or less means all of them.
         */
        long limit{};

        /**
         * @brief How many matching documents to step over before returning any.
         */
        long skip{};
    };

    /**
     * @brief One group of a grouped count - see DocumentStore::GroupCount.
     */
    struct GroupCounts {

        /**
         * @brief The values of the fields grouped on, in the order they were asked for.
         */
        std::vector<std::string> key;

        /**
         * @brief How many documents fell into this group.
         */
        long count{};

        /**
         * @brief The sum of the numeric field the caller named, or zero if it named none.
         */
        long sum{};
    };

}// namespace Euclid::Database::Emd
