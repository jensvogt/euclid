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
    }

    void Database::initializeRemote(const std::string &socketPath) {
        _store = std::make_shared<Emd::RemoteDocumentStore>(socketPath);
        _databaseName = "euclid";
        log_info << "Using the memory database, socket: " << socketPath;
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
