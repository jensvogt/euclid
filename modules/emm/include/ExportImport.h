// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <string>

// Boost includes
#include <boost/json.hpp>

// Euclid includes
#include <euclid/database/Collection.h>

namespace Euclid::EMM {

    /**
     * @brief What one collection's import did.
     */
    struct ImportOutcome {

        /**
         * @brief Documents written.
         */
        long imported{};

        /**
         * @brief Documents that could not be, each for its own reason - see ImportCollection().
         */
        long failed{};
    };

    /**
     * @brief Renders one collection as a JSON array of its documents.
     *
     * @par
     * Each document round-trips through bsoncxx's relaxed Extended JSON (ISO dates, plain numbers,
     * {"$oid": ...} for ObjectIds) on the way to a boost::json::value, rather than being
     * hand-assembled from known fields - an export is meant to reflect whatever is actually stored,
     * including fields no repository method happens to read back out.
     *
     * @param collection the collection to read.
     * @return its documents, in the order the store returns them.
     */
    [[nodiscard]]
    boost::json::array ExportCollection(const Database::Collection &collection);

    /**
     * @brief Upserts one collection's worth of documents, by "_id".
     *
     * @par
     * Replaces the whole document rather than merging fields, so a restore reflects the export
     * exactly - including fields since removed from the live document. Each document is parsed
     * independently so one malformed entry - the file is external input, possibly hand-edited -
     * fails on its own instead of aborting the rest of the collection.
     *
     * @param collection the collection to write.
     * @param name       its name, for the log line about a document that could not be written.
     * @param docs       the documents, as an export wrote them.
     * @return how many were written, and how many were not.
     */
    [[nodiscard]]
    ImportOutcome ImportCollection(const Database::Collection &collection, const std::string &name, const boost::json::array &docs);

}// namespace Euclid::EMM
