// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 5/24/26.
//

// C++ includes
#include <string>

// Mongodb includes
#include <bsoncxx/document/view.hpp>
#include <bsoncxx/types.hpp>

// Euclid includes
#include <euclid/database/Database.h>
#include <euclid/database/emd/DocumentStore.h>
#include <euclid/database/emd/RemoteDocumentStore.h>

namespace Euclid::Database {

    namespace {

        /**
         * @brief The host part of the connection URI: every seed, each carrying a port.
         *
         * @par
         * More than one host because a replica set's primary moves, and the seeds are only where
         * the driver starts looking for it - see the replica-set note in initialize(). Configuration
         * gives them as one comma-separated string, and a seed may name its own port, for a set
         * whose members do not share one.
         *
         * @param hosts comma-separated host list, each entry "host" or "host:port".
         * @param defaultPort the port for the seeds that do not name one.
         * @return the seed list for the URI, empty if the configured list named no host.
         */
        std::string seedList(const std::string &hosts, const std::string &defaultPort) {

            std::string seeds;

            for (std::size_t pos = 0; pos <= hosts.size();) {

                const auto comma = hosts.find(',', pos);
                auto seed = hosts.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                pos = comma == std::string::npos ? hosts.size() + 1 : comma + 1;

                const auto first = seed.find_first_not_of(" \t");
                if (first == std::string::npos) continue;
                seed = seed.substr(first, seed.find_last_not_of(" \t") - first + 1);

                // From past the closing bracket of an IPv6 literal, whose own colons are not a port.
                const auto hostEnd = seed.front() == '[' ? seed.find(']') : 0;
                if (seed.find(':', hostEnd) == std::string::npos) seed += ":" + defaultPort;

                if (!seeds.empty()) seeds += ",";
                seeds += seed;
            }

            return seeds;
        }
    }

    void Database::initialize() {

        const auto host = Core::Configuration::instance().getOr<std::string>("euclid.mongodb.host", "localhost");
        const auto port = Core::Configuration::instance().getOr<std::string>("euclid.mongodb.port", "27017");
        const auto user = Core::Configuration::instance().getOr<std::string>("euclid.mongodb.user", "root");
        const auto password = Core::Configuration::instance().getOr<std::string>("euclid.mongodb.password", "password");
        const auto replicaSet = Core::Configuration::instance().getOr<std::string>("euclid.mongodb.replica-set", "");
        const int selectionTimeout = Core::Configuration::instance().getOr<int>("euclid.mongodb.server-selection-timeout-ms", 0);
        const int poolSize = Core::Configuration::instance().getOr<int>("euclid.mongodb.pool-size", 10);
        _databaseName = Core::Configuration::instance().getOr<std::string>("euclid.mongodb.name", "euclid");

        const auto seeds = seedList(host, port);
        if (seeds.empty()) {
            throw std::runtime_error("MongoDB initialization failed: euclid.mongodb.host names no host");
        }

        try {

            // Embed pool size in the URI — works for all mongocxx versions
            std::string options = "/?maxPoolSize=" + std::to_string(poolSize);

            // Named, so the driver reads the seeds as one replica set rather than as hosts that
            // happen to be listed together: it discovers the rest of the members, watches for
            // elections, and sends every write to whichever member is primary now. Without it the
            // driver talks to the one host it was handed and nothing else, so an election that
            // moves the primary elsewhere leaves every write failing "not primary" until somebody
            // restarts the process - and nothing recovers on its own, because in that topology the
            // driver never goes looking for the primary in the first place.
            if (!replicaSet.empty()) options += "&replicaSet=" + replicaSet;

            // Otherwise the driver's own 30s, which is long enough that an ordinary election
            // finishes inside it and the operations waiting on one never see it. Worth shortening
            // only where something would rather be told quickly that there is no primary than wait
            // for one to turn up.
            if (selectionTimeout > 0) options += "&serverSelectionTimeoutMS=" + std::to_string(selectionTimeout);

            mongocxx::uri _uri("mongodb://" + user + ":" + password + "@" + seeds + options);

            // options::pool exists but has no size method in newer versions
            _pool = std::make_unique<mongocxx::pool>(_uri);

            log_debug << "MongoDB pool initialized" << " uri=" << _uri.to_string() << " database=" << _databaseName << " poolSize=" << poolSize;

            ping();

            // It answered a ping, so anything waiting for a reachable backend can run now - which
            // on this backend is what it always did, from the repository's own constructor.
            markReachable();

        } catch (const mongocxx::exception &e) {
            log_error << "MongoDB initialization failed: " << e.what();
            throw std::runtime_error(std::string("MongoDB initialization failed: ") + e.what());
        }
    }

    mongocxx::pool::entry Database::client() const {
        if (!_pool) {
            throw std::runtime_error("MongoDB not initialized — call initialize() first");
        }
        return _pool->acquire();
    }

    void Database::ping() const {

        // The store answers for its own reachability. For the shared one that is a real round trip
        // to the EMD process, which is what the manager's watchdog wants to know; for an in-process
        // store it is trivially true. Without this the watchdog dereferenced a connection pool that
        // a non-MongoDB backend never created.
        if (_store) {
            std::ignore = _store->CountDocuments("euclid_ping", bsoncxx::document::view{});
            return;
        }

        if (!_pool) {
            throw std::runtime_error("MongoDB not initialized — call initialize() first");
        }

        try {
            const auto entry = _pool->acquire();
            auto db = (*entry)["admin"];

            // "hello" rather than "ping", because a secondary answers a ping exactly as happily as
            // a primary does. On a set that had lost its primary that made this say reachable:
            // everything waiting on it ran, every one of their writes failed "not primary", and the
            // installation came up looking healthy while being unable to record a single thing. What
            // a caller means by reachable is that a primary exists and this process is talking to
            // it, which is what isWritablePrimary answers - called ismaster before MongoDB 5.0.
            const auto cmd = bsoncxx::builder::stream::document{} << "hello" << 1 << bsoncxx::builder::stream::finalize;

            const auto reply = db.run_command(cmd.view());
            const auto view = reply.view();

            const auto writable = view["isWritablePrimary"] ? view["isWritablePrimary"] : view["ismaster"];
            if (!writable || writable.type() != bsoncxx::type::k_bool || !writable.get_bool().value) {
                throw std::runtime_error("no writable primary");
            }

            log_trace << "MongoDB ping successful";

        } catch (const mongocxx::exception &e) {
            log_error << "MongoDB ping failed: " << e.what();
            throw std::runtime_error(std::string("MongoDB ping failed: ") + e.what());
        } catch (const std::exception &e) {
            log_error << "MongoDB ping failed: " << e.what();
            throw;
        }
    }

    void Database::initializeMemory() {
        _store = std::make_shared<Emd::DocumentStore>();
        _databaseName = "euclid";
        log_info << "Using the in-memory document store";

        // In this process, so up the moment it exists.
        markReachable();
    }

    void Database::initializeRemote(const std::string &socketPath) {

        auto store = std::make_shared<Emd::RemoteDocumentStore>(socketPath);

        // Deliberately not marked reachable here: EMD is another process, and on the installation
        // this backend exists for the manager initializes this before it has started it. The store
        // says when it has actually been answered, which is what anything deferred is waiting for.
        store->OnReply([this] { markReachable(); });

        _store = std::move(store);
        _databaseName = "euclid";
        log_info << "Using the memory database, socket: " << socketPath;
    }

    void Database::onReachable(std::function<void()> work) {

        {
            std::lock_guard lock(_reachableMutex);
            if (!_reachable.load(std::memory_order_relaxed)) {
                _deferred.push_back(std::move(work));
                return;
            }
        }

        // Outside the lock: the work makes store calls, and one of those answering re-enters
        // markReachable() - which wants this same mutex.
        work();
    }

    void Database::markReachable() {

        // The steady state, and the reason this is cheap enough to call on every reply.
        if (_reachable.load(std::memory_order_acquire)) return;

        std::vector<std::function<void()> > deferred;
        {
            std::lock_guard lock(_reachableMutex);
            if (_reachable.exchange(true, std::memory_order_release)) return;
            deferred.swap(_deferred);
        }

        if (!deferred.empty()) {
            log_debug << "The backend answered; running " << deferred.size() << " deferred initialisation(s)";
        }
        for (const auto &work: deferred) work();
    }

    Collection Database::collection(const std::string &name) const {

        // The store first: a process initialized with one has no pool to acquire from, and asking
        // would throw rather than fall back.
        if (_store) return Collection(_store, name);
        if (!_pool) {
            throw std::runtime_error("MongoDB not initialized — call initialize() first");
        }
        return Collection(_pool->acquire(), _databaseName, name);
    }

}// namespace Euclid::Core
