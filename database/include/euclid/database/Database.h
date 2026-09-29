// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 5/24/26.
//
#pragma once

// C++ includes
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Mongodb includes
#include <mongocxx/instance.hpp>
#include <mongocxx/pool.hpp>
#include <bsoncxx/builder/stream/document.hpp>
#include <mongocxx/v_noabi/mongocxx/exception/exception.hpp>
#include <mongocxx/v_noabi/mongocxx/uri.hpp>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/core/LogStream.h>
#include <euclid/database/Collection.h>

namespace Euclid::Database {

    class Database {

    public:
        static Database &instance() {
            static Database inst;
            return inst;
        }

        Database(const Database &) = delete;

        Database &operator=(const Database &) = delete;

        /**
         * @brief Initialize the MongoDB connection pool
         */
        void initialize();

        /**
         * @brief Serve every collection from an in-memory document store instead of MongoDB.
         *
         * @par
         * The store keeps MongoDB's semantics - see Emd::DocumentStore - so the repositories are
         * the same code either way. This one lives inside the calling process, which suits a test
         * that exercises a single module; the EMD module serves one store to all of them.
         */
        void initializeMemory();

        /**
         * @brief Serve every collection from the document store the EMD module holds.
         *
         * @par
         * The same semantics as the in-process store, on the other side of a socket - so every
         * module in the installation reads and writes one set of documents, which is what makes
         * this a backend an installation can actually run on rather than a per-process scratchpad.
         *
         * @param socketPath the socket EMD listens on.
         */
        void initializeRemote(const std::string &socketPath);

        /**
         * @brief One collection, on whichever backend this process was initialized with.
         *
         * @par
         * What a repository asks for instead of reaching into the connection pool itself. That is
         * the whole seam: two lines at the top of a repository method decide the backend, and
         * everything below is written against MongoDB's own API whatever answers it.
         */
        [[nodiscard]]
        Collection collection(const std::string &name) const;

        /**
         * @brief Runs work as soon as the backend answers, immediately if it already does.
         *
         * @par
         * What a repository's index creation goes through. On MongoDB and the in-process store the
         * backend is up by the time this process is initialized, so the work runs inline and this
         * is the call it always was. On the EMD backend it need not be: the manager constructs its
         * repositories before it starts the module that holds the store, so the indexes were
         * attempted against a socket that did not exist yet, failed, and - being a constructor -
         * were never attempted again. A missing index is silent afterwards, which is the worst
         * shape for this to fail in: one error line at startup and no uniqueness for the life of
         * the process.
         *
         * @par
         * Runs at most once. Deferred work is run on whichever thread first gets an answer out of
         * the store, after that request has finished with its own connection - so the work is free
         * to make store calls of its own, which index creation does.
         *
         * @param work what to run. Anything it throws is caught and logged by the caller's own
         * handler as it would have been inline; it is not retried again after it has run.
         */
        void onReachable(std::function<void()> work);

        /**
         * @brief Whether this process is served by the in-memory store rather than MongoDB.
         *
         * @par
         * For the handful of places that cannot be expressed as a collection operation at all -
         * a database-level command, an aggregation pipeline - and have to say what they can do
         * instead.
         */
        [[nodiscard]]
        bool inMemory() const { return _store != nullptr; }

        /**
         * @brief The in-memory store, or null on MongoDB.
         */
        [[nodiscard]]
        std::shared_ptr<Emd::IDocumentStore> store() const { return _store; }

        /**
         * @brief Acquire a client from the pool
         */
        mongocxx::pool::entry client() const;

        /**
         * @brief Returns the database name
         */
        [[nodiscard]]
        const std::string &databaseName() const { return _databaseName; }

        /**
         * @brief Ping the server — throws if unreachable
         */
        void ping() const;

    private:
        Database() = default;

        /**
         * @brief Records that the backend is answering, and runs whatever waited for that.
         *
         * @par
         * Called on every successful store reply, so the first line is the one that matters: after
         * the first it is a single atomic read, which is what keeps it off the hot path.
         */
        void markReachable();

        // mongocxx::instance must be created exactly once per process
        mongocxx::instance _instance{};
        std::unique_ptr<mongocxx::pool> _pool{};
        std::string _databaseName;

        /**
         * @brief Whether the backend has answered, and what is waiting for it to.
         */
        std::atomic_bool _reachable{false};
        std::mutex _reachableMutex;
        std::vector<std::function<void()> > _deferred;

        /**
         * @brief The in-memory store, when this process was initialized with one; null when it
         * talks to MongoDB.
         */
        std::shared_ptr<Emd::IDocumentStore> _store;
    };

} // namespace Euclid::Core