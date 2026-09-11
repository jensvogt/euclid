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
#include <bsoncxx/document/view.hpp>
#include <bsoncxx/oid.hpp>

// Euclid includes
#include <euclid/database/emd/StoreTypes.h>

namespace Euclid::Database::Emd {

    /**
     * @brief What a document store offers, wherever it lives.
     *
     * @par
     * Two implementations: the store itself, and a client that forwards every call to the EMD
     * process over its socket. That is the whole difference between "in memory in this process"
     * and "in memory in one process, shared by all of them" - the repositories, the collections
     * and the semantics above this line are identical either way.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class IDocumentStore {

    public:

        virtual ~IDocumentStore() = default;
        /**
         * @brief The first document matching the filter, or nothing.
         */
        [[nodiscard]]
        virtual std::optional<bsoncxx::document::value> FindOne(const std::string &collection, bsoncxx::document::view filter) const = 0;

        /**
         * @brief Every document matching the filter, sorted, skipped and limited as asked.
         */
        [[nodiscard]]
        virtual std::vector<bsoncxx::document::value> Find(const std::string &collection, bsoncxx::document::view filter,
                                                   const FindOptions &options = {}) const = 0;

        /**
         * @brief Stores a document, giving it an "_id" if it has none.
         *
         * @return the id it was stored under.
         * @throws std::runtime_error if it would duplicate a unique index.
         */
        virtual bsoncxx::oid InsertOne(const std::string &collection, bsoncxx::document::view document) = 0;

        /**
         * @brief Applies an update to the first matching document, optionally creating one.
         *
         * @param collection collection name
         * @param filter selects the document
         * @param update "$set", "$setOnInsert", "$currentDate", "$inc", "$push" or "$pull"
         * @param upsert whether to create a document when nothing matched
         * @throws std::runtime_error if the update names an operator this store does not
         * implement - refused rather than ignored, because an update that silently did nothing is
         * how a store like this quietly loses data.
         */
        virtual UpdateResult UpdateOne(const std::string &collection, bsoncxx::document::view filter,
                                       bsoncxx::document::view update, bool upsert = false) = 0;

        /**
         * @brief Applies an update to every matching document.
         */
        virtual UpdateResult UpdateMany(const std::string &collection, bsoncxx::document::view filter,
                                        bsoncxx::document::view update) = 0;

        /**
         * @brief Replaces the first matching document wholesale, keeping its id.
         */
        virtual UpdateResult ReplaceOne(const std::string &collection, bsoncxx::document::view filter,
                                        bsoncxx::document::view replacement, bool upsert = false) = 0;

        /**
         * @brief Updates the first matching document and returns it.
         *
         * @param returnAfter true to return the document as it is now, false as it was - the
         * driver's returnDocument option, which the repositories set to "after" so that an upsert
         * hands back the row it just wrote.
         */
        virtual std::optional<bsoncxx::document::value> FindOneAndUpdate(const std::string &collection, bsoncxx::document::view filter,
                                                                 bsoncxx::document::view update, bool upsert, bool returnAfter) = 0;

        /**
         * @brief Removes the first matching document and returns it.
         */
        virtual std::optional<bsoncxx::document::value> FindOneAndDelete(const std::string &collection, bsoncxx::document::view filter) = 0;

        /**
         * @brief Removes matching documents.
         *
         * @param single true to remove at most one.
         * @return how many were removed.
         */
        virtual long Delete(const std::string &collection, bsoncxx::document::view filter, bool single = false) = 0;

        /**
         * @brief How many documents match.
         */
        [[nodiscard]]
        virtual long CountDocuments(const std::string &collection, bsoncxx::document::view filter) const = 0;

        /**
         * @brief Counts and sums matching documents, grouped by one or more fields.
         *
         * @par
         * The one shape of aggregation the repositories need - "how many messages, and how many
         * bytes, per queue and status" - as a method rather than a pipeline. Everything else about
         * aggregation stays where it is: the pipelines that do real work (the monitoring rollups)
         * are asked of MongoDB, and their EMD counterparts are written in C++ against Find().
         *
         * @param collection collection name
         * @param filter narrows what is counted; an empty document counts everything
         * @param groupFields fields whose values identify a group, in order
         * @param sumField numeric field to total per group, or empty to only count
         */
        [[nodiscard]]
        virtual std::vector<GroupCounts> GroupCount(const std::string &collection, bsoncxx::document::view filter,
                                            const std::vector<std::string> &groupFields, const std::string &sumField = {}) const = 0;

        /**
         * @brief Records that a set of fields must be unique.
         *
         * @par
         * Enforced on insert and upsert, like the real thing: euclid creates these indexes to keep
         * two rows from claiming one name, and a store that accepted the duplicate would let a
         * test pass that production would refuse.
         *
         * @param collection collection name
         * @param keys index specification, e.g. { "accountId": 1, "name": 1 }
         * @param unique whether the index is unique; a non-unique index is remembered and
         * otherwise does nothing, since nothing here needs one to be fast.
         */
        virtual void CreateIndex(const std::string &collection, bsoncxx::document::view keys, bool unique) = 0;

        /**
         * @brief Forgets everything. For a test that wants a clean store without a new process.
         */
        virtual void Clear() = 0;

        /**
         * @brief The names of the collections that hold at least one document.
         */
        [[nodiscard]]
        virtual std::vector<std::string> Collections() const = 0;

        /**
         * @brief How many documents a collection holds, for reporting.
         */
        [[nodiscard]]
        virtual long Size(const std::string &collection) const = 0;

    };

}// namespace Euclid::Database::Emd
