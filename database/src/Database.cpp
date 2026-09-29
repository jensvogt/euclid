// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 5/24/26.
//

#include <euclid/database/Database.h>
#include <euclid/database/emd/DocumentStore.h>
#include <euclid/database/emd/RemoteDocumentStore.h>

namespace Euclid::Database {

    void Database::initialize() {

        const auto host = Core::Configuration::instance().getOr<std::string>("euclid.mongodb.host", "localhost");
        const auto port = Core::Configuration::instance().getOr<std::string>("euclid.mongodb.port", "27017");
        const auto user = Core::Configuration::instance().getOr<std::string>("euclid.mongodb.user", "root");
        const auto password = Core::Configuration::instance().getOr<std::string>("euclid.mongodb.password", "password");
        const int poolSize = Core::Configuration::instance().getOr<int>("euclid.mongodb.pool-size", 10);
        _databaseName = Core::Configuration::instance().getOr<std::string>("euclid.mongodb.name", "euclid");

        try {

            // Embed pool size in the URI — works for all mongocxx versions
            mongocxx::uri _uri("mongodb://" + user + ":" + password + "@" + host + ":" + port + "/?maxPoolSize=" + std::to_string(poolSize));

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
            auto db = (*entry)[_databaseName];

            const auto cmd = bsoncxx::builder::stream::document{} << "ping" << 1 << bsoncxx::builder::stream::finalize;

            db.run_command(cmd.view());
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
