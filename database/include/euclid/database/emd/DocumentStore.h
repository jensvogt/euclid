// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/8/26.
//

#pragma once

// C++ includes
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// MongoDB includes
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view.hpp>
#include <bsoncxx/oid.hpp>
#include <bsoncxx/types/bson_value/value.hpp>

// Euclid includes
#include <euclid/database/emd/IDocumentStore.h>

namespace Euclid::Database::Emd {

    /**
     * @brief An in-memory document database with MongoDB's semantics.
     *
     * @par
     * euclid's domain layer is written against MongoDB: entities serialize themselves to BSON,
     * repositories filter with "$regex" and upsert with "$set"/"$setOnInsert"/"$currentDate". The
     * in-memory alternative was a second implementation of every repository - three thousand lines
     * that had to be kept in step with five thousand by hand, and quietly did not (an upsert that
     * only ever inserted, for one). Worse, it lived inside whichever process created it, so a
     * login issued by EAM was invisible to the module the caller went on to talk to.
     *
     * @par
     * So this is the other way round: one store that behaves the way the repositories already
     * expect, served to every module over a socket, and one domain implementation for both
     * backends. What it deliberately is not is a MongoDB: it speaks euclid's own protocol rather
     * than the wire protocol, implements the operators this codebase actually uses rather than all
     * of them, and keeps everything in memory, so it is for tests, demonstrations and a container
     * that has no database beside it.
     *
     * @par
     * Every method is safe to call from several threads. The lock is a single mutex over the whole
     * store, which is the right trade for something that holds thousands of documents rather than
     * millions and is asked for them over a socket anyway.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class DocumentStore final : public IDocumentStore {

    public:

        DocumentStore() = default;

        DocumentStore(const DocumentStore &) = delete;
        DocumentStore &operator=(const DocumentStore &) = delete;

        /**
         * @brief The first document matching the filter, or nothing.
         */
        [[nodiscard]]
        std::optional<bsoncxx::document::value> FindOne(const std::string &collection, bsoncxx::document::view filter) const override;

        /**
         * @brief Every document matching the filter, sorted, skipped and limited as asked.
         */
        [[nodiscard]]
        std::vector<bsoncxx::document::value> Find(const std::string &collection, bsoncxx::document::view filter,
                                                   const FindOptions &options = {}) const override;

        /**
         * @brief Stores a document, giving it an "_id" if it has none.
         *
         * @return the id it was stored under.
         * @throws std::runtime_error if it would duplicate a unique index.
         */
        bsoncxx::oid InsertOne(const std::string &collection, bsoncxx::document::view document) override;

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
        UpdateResult UpdateOne(const std::string &collection, bsoncxx::document::view filter,
                               bsoncxx::document::view update, bool upsert = false) override;

        /**
         * @brief Applies an update to every matching document.
         */
        UpdateResult UpdateMany(const std::string &collection, bsoncxx::document::view filter,
                                bsoncxx::document::view update) override;

        /**
         * @brief Replaces the first matching document wholesale, keeping its id.
         */
        UpdateResult ReplaceOne(const std::string &collection, bsoncxx::document::view filter,
                                bsoncxx::document::view replacement, bool upsert = false) override;

        /**
         * @brief Updates the first matching document and returns it.
         *
         * @param returnAfter true to return the document as it is now, false as it was - the
         * driver's returnDocument option, which the repositories set to "after" so that an upsert
         * hands back the row it just wrote.
         */
        std::optional<bsoncxx::document::value> FindOneAndUpdate(const std::string &collection, bsoncxx::document::view filter,
                                                                 bsoncxx::document::view update, bool upsert, bool returnAfter) override;

        /**
         * @brief Removes the first matching document and returns it.
         */
        std::optional<bsoncxx::document::value> FindOneAndDelete(const std::string &collection, bsoncxx::document::view filter) override;

        /**
         * @brief Removes matching documents.
         *
         * @param single true to remove at most one.
         * @return how many were removed.
         */
        long Delete(const std::string &collection, bsoncxx::document::view filter, bool single = false) override;

        /**
         * @brief How many documents match.
         */
        [[nodiscard]]
        long CountDocuments(const std::string &collection, bsoncxx::document::view filter) const override;

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
        std::vector<GroupCounts> GroupCount(const std::string &collection, bsoncxx::document::view filter,
                                            const std::vector<std::string> &groupFields, const std::string &sumField = {}) const override;

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
        void CreateIndex(const std::string &collection, bsoncxx::document::view keys, bool unique) override;

        /**
         * @brief Forgets everything. For a test that wants a clean store without a new process.
         */
        void Clear() override;

        /**
         * @brief The names of the collections that hold at least one document.
         */
        [[nodiscard]]
        std::vector<std::string> Collections() const override;

        /**
         * @brief How many documents a collection holds, for reporting.
         */
        [[nodiscard]]
        long Size(const std::string &collection) const override;

    private:

        /**
         * @brief One collection: its documents in insertion order, and the unique indexes that
         * constrain them.
         */
        struct Collection {
            std::vector<bsoncxx::document::value> documents;
            std::vector<std::vector<std::string> > uniqueKeys;
        };

        /**
         * @brief Refuses an insert that would duplicate a unique index. Called with the lock held.
         *
         * @param collection the collection being written
         * @param document the document about to be stored
         * @param replacingIndex index of the document being replaced, or -1 for an insert, so a
         * document does not collide with itself.
         */
        static void CheckUnique(const Collection &collection, bsoncxx::document::view document, long replacingIndex);

        mutable std::mutex _mutex;
        std::map<std::string, Collection> _collections;
    };

}// namespace Euclid::Database::Emd
