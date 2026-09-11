// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/8/26.
//

#pragma once

// C++ includes
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// MongoDB includes
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view_or_value.hpp>
#include <bsoncxx/oid.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/find_one_and_update.hpp>
#include <mongocxx/options/index.hpp>
#include <mongocxx/options/update.hpp>
#include <mongocxx/pipeline.hpp>
#include <mongocxx/pool.hpp>

// Euclid includes
#include <euclid/database/emd/IDocumentStore.h>

namespace Euclid::Database {

    /**
     * @brief The documents a find returned, iterable once, in order.
     *
     * @par
     * Yields views rather than values so that a loop written for mongocxx's own cursor - which is
     * every loop in this codebase - compiles unchanged. The documents themselves belong to the
     * cursor, so it has to outlive the loop, which is how a cursor is used anyway.
     */
    class DocumentCursor {

    public:

        explicit DocumentCursor(std::vector<bsoncxx::document::value> documents) : _documents(std::move(documents)) {}

        class iterator {

        public:
            using value_type = bsoncxx::document::view;
            using difference_type = std::ptrdiff_t;

            iterator(const std::vector<bsoncxx::document::value> *documents, const std::size_t index) : _documents(documents), _index(index) {}

            bsoncxx::document::view operator*() const { return (*_documents)[_index].view(); }
            iterator &operator++() {
                ++_index;
                return *this;
            }
            bool operator!=(const iterator &other) const { return _index != other._index; }
            bool operator==(const iterator &other) const { return _index == other._index; }

        private:
            const std::vector<bsoncxx::document::value> *_documents;
            std::size_t _index;
        };

        [[nodiscard]] iterator begin() const { return {&_documents, 0}; }
        [[nodiscard]] iterator end() const { return {&_documents, _documents.size()}; }

    private:
        std::vector<bsoncxx::document::value> _documents;
    };

    /**
     * @brief What an update did, in the shape the driver reports it.
     */
    class UpdateOutcome {

    public:
        UpdateOutcome(const long matched, const long modified, std::optional<bsoncxx::oid> upserted)
            : _matched(matched), _modified(modified), _upserted(std::move(upserted)) {}

        [[nodiscard]] std::int32_t matched_count() const { return static_cast<std::int32_t>(_matched); }
        [[nodiscard]] std::int32_t modified_count() const { return static_cast<std::int32_t>(_modified); }
        [[nodiscard]] std::optional<bsoncxx::oid> upserted_id() const { return _upserted; }

    private:
        long _matched;
        long _modified;
        std::optional<bsoncxx::oid> _upserted;
    };

    /**
     * @brief What a delete did.
     */
    class DeleteOutcome {

    public:
        explicit DeleteOutcome(const long deleted) : _deleted(deleted) {}
        [[nodiscard]] std::int32_t deleted_count() const { return static_cast<std::int32_t>(_deleted); }

    private:
        long _deleted;
    };

    /**
     * @brief One collection, on whichever database this process was pointed at.
     *
     * @par
     * Deliberately the same methods, arguments and option types as mongocxx::collection, for the
     * subset euclid uses. That is what makes a repository backend-agnostic without being rewritten:
     * the two lines that acquire a collection change, and the two hundred that use it do not. It
     * also means a repository keeps being written against MongoDB's semantics, which is the right
     * way round - the emulation follows the database, not the other way about.
     *
     * @par
     * Backed either by a real MongoDB collection - holding the pool handle for its own lifetime,
     * so the connection cannot be returned while a cursor is still reading - or by an in-memory
     * document store. Everything that differs between the two lives here.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class Collection {

    public:

        /**
         * @brief A collection on MongoDB, holding the pool handle it was taken from.
         */
        Collection(mongocxx::pool::entry entry, const std::string &databaseName, const std::string &name);

        /**
         * @brief A collection in an in-memory store.
         */
        Collection(std::shared_ptr<Emd::IDocumentStore> store, std::string name);

        [[nodiscard]]
        std::optional<bsoncxx::document::value> find_one(bsoncxx::document::view_or_value filter) const;

        [[nodiscard]]
        DocumentCursor find(bsoncxx::document::view_or_value filter, const mongocxx::options::find &options = {}) const;

        std::optional<bsoncxx::oid> insert_one(bsoncxx::document::view_or_value document) const;

        std::optional<UpdateOutcome> update_one(bsoncxx::document::view_or_value filter, bsoncxx::document::view_or_value update,
                                                const mongocxx::options::update &options = {}) const;

        std::optional<UpdateOutcome> update_many(bsoncxx::document::view_or_value filter, bsoncxx::document::view_or_value update,
                                                 const mongocxx::options::update &options = {}) const;

        /**
         * @brief Applies an aggregation-pipeline update to every matching document.
         *
         * @par
         * MongoDB only - it is the server that evaluates the pipeline. A caller asks
         * @ref supports_aggregation first and does the same work with find() and update_one()
         * when there is no server to ask, which is what ESM's bucket rename does.
         *
         * @throws std::runtime_error when this collection is in memory.
         */
        std::optional<UpdateOutcome> update_many(bsoncxx::document::view_or_value filter, const mongocxx::pipeline &update) const;

        std::optional<UpdateOutcome> replace_one(bsoncxx::document::view_or_value filter, bsoncxx::document::view_or_value replacement,
                                                 const mongocxx::options::replace &options = {}) const;

        std::optional<bsoncxx::document::value> find_one_and_update(bsoncxx::document::view_or_value filter, bsoncxx::document::view_or_value update,
                                                                    const mongocxx::options::find_one_and_update &options = {}) const;

        std::optional<bsoncxx::document::value> find_one_and_delete(bsoncxx::document::view_or_value filter) const;

        std::optional<DeleteOutcome> delete_one(bsoncxx::document::view_or_value filter) const;

        std::optional<DeleteOutcome> delete_many(bsoncxx::document::view_or_value filter) const;

        [[nodiscard]]
        std::int64_t count_documents(bsoncxx::document::view_or_value filter) const;

        /**
         * @brief Creates an index.
         *
         * @par
         * The options are taken by non-const reference because the driver takes them that way and
         * they cannot be copied - an index option can own storage-engine settings. Call sites pass
         * a local, which is what they already did.
         */
        void create_index(bsoncxx::document::view_or_value keys, mongocxx::options::index &options) const;

        /**
         * @brief Creates an index with default options.
         */
        void create_index(bsoncxx::document::view_or_value keys) const;

        /**
         * @brief Runs an aggregation pipeline.
         *
         * @par
         * MongoDB only. The pipelines euclid runs are the monitoring rollups, which use
         * "$dateTrunc" and "$merge" and are not worth an aggregation engine here; a repository
         * that needs one asks @ref supports_aggregation first and computes the answer from find()
         * when there is none.
         *
         * @throws std::runtime_error when this collection is in memory.
         */
        [[nodiscard]]
        DocumentCursor aggregate(const mongocxx::pipeline &pipeline) const;

        /**
         * @brief Whether aggregate() will work, i.e. whether this is a real MongoDB.
         */
        [[nodiscard]]
        bool supports_aggregation() const { return _collection.has_value(); }

        /**
         * @brief Counts and sums matching documents grouped by fields, on either backend.
         *
         * @par
         * The one aggregation shape the repositories genuinely need - the recounts that rebuild a
         * queue's or a bucket's totals - expressed as a method so that both backends can answer
         * it: MongoDB with a "$group" pipeline, the in-memory store by walking its documents.
         *
         * @param filter narrows what is counted
         * @param groupFields fields whose values identify a group, in order
         * @param sumField numeric field to total per group, or empty to only count
         */
        [[nodiscard]]
        std::vector<Emd::GroupCounts> group_count(bsoncxx::document::view_or_value filter, const std::vector<std::string> &groupFields,
                                                  const std::string &sumField = {}) const;

    private:

        /**
         * @brief The pool handle the MongoDB collection came from, held so the client outlives it.
         */
        std::optional<mongocxx::pool::entry> _entry;

        /**
         * @brief The MongoDB collection, or nothing when this collection is in memory.
         *
         * @par
         * Mutable because the driver's own methods are non-const - a find changes nothing about
         * the collection object, but it does talk to a socket, and the driver does not promise
         * otherwise. Keeping the methods here const is what lets a repository hold one by value
         * in a const method, which most of them do.
         */
        mutable std::optional<mongocxx::collection> _collection;

        /**
         * @brief The in-memory store, or null when this collection is on MongoDB.
         */
        std::shared_ptr<Emd::IDocumentStore> _store;

        /**
         * @brief Collection name, which the in-memory store keys on.
         */
        std::string _name;
    };

}// namespace Euclid::Database
