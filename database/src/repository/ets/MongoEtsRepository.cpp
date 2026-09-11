// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/31/26.
//

#include <bsoncxx/builder/basic/array.hpp>
#include <euclid/database/repository/ets/MongoEtsRepository.h>

namespace Euclid::Database {

    MongoEtsRepository::MongoEtsRepository() {
        ensureIndexes();
    }

    void MongoEtsRepository::ensureIndexes() {

        try {
            auto collection = Database::instance().collection(COLLECTION);

            // Compound on (accountId, namespace, serverId) rather than serverId alone - a server
            // is named within its account and namespace, like every other resource. What has to
            // stay unique installation-wide is the name it *runs* under, which is a name of its
            // own and has an index of its own below.
            //
            // NOTE: replacing a pre-existing unique index on "serverId" alone requires dropping
            // that old index first (db.ets_transfer_server.dropIndex("serverId_1")) - Mongo won't
            // do it itself.
            mongocxx::options::index serverIdOpts;
            serverIdOpts.unique(true);
            collection.create_index(make_document(kvp("accountId", 1), kvp("namespace", 1), kvp("serverId", 1)), serverIdOpts);

            mongocxx::options::index ernOpts;
            ernOpts.unique(true);
            collection.create_index(make_document(kvp("ern", 1)), ernOpts);

            // What the manager, the module registry and the spawned process itself know a server
            // by - a process pool, a unix socket and a --transfer-server argument have no account
            // or namespace to be unique within.
            //
            // Sparse, because a server created before the field existed has none: it runs under
            // its bare serverId, which the compound index above already keeps unique within its
            // account and namespace. A sparse index skips those rather than reading them all as
            // one shared empty name.
            mongocxx::options::index runtimeNameOpts;
            runtimeNameOpts.unique(true);
            runtimeNameOpts.sparse(true);
            collection.create_index(make_document(kvp("runtimeName", 1)), runtimeNameOpts);

        } catch (const std::exception &e) {
            log_error << "Ensure transfer server indexes failed, error: " << e.what();
        }
    }

    bsoncxx::document::value MongoEtsRepository::serverFilter(const std::string &accountId, const std::string &nameSpace,
                                                              const std::string &serverId) {
        // The three fields the unique index is built on, in its order.
        return make_document(kvp("accountId", accountId), kvp("namespace", nameSpace), kvp("serverId", serverId));
    }

    Entity::ETS::TransferServer MongoEtsRepository::upsertServer(Entity::ETS::TransferServer &server) {

        try {
            auto collection = Database::instance().collection(COLLECTION);

            // created is carried in toDocument(), so it is stamped here on the insert path
            // rather than through $setOnInsert - which would collide with the same field in $set.
            const auto now = std::chrono::system_clock::now();
            if (server.created.time_since_epoch().count() == 0) {
                server.created = now;
            }
            server.modified = now;

            mongocxx::options::find_one_and_update opts;
            opts.upsert(true);
            opts.return_document(mongocxx::options::return_document::k_after);

            // Matches the unique index: filtered on the serverId alone this would find another
            // account's server of that name and overwrite its definition.
            const auto filter = serverFilter(server.accountId, server.nameSpace, server.serverId);
            if (auto result = collection.find_one_and_update(filter.view(),
                                                             make_document(kvp("$set", server.toDocument())).view(), opts)) {
                return Entity::ETS::TransferServer::fromDocument(result->view());
            }

        } catch (const std::exception &e) {
            log_error << "Upsert transfer server failed, error: " << e.what();
        }
        return server;
    }

    std::optional<Entity::ETS::TransferServer> MongoEtsRepository::findServerByServerId(const std::string &accountId, const std::string &nameSpace,
                                                                                        const std::string &serverId) const {

        try {
            auto collection = Database::instance().collection(COLLECTION);

            if (const auto result = collection.find_one(serverFilter(accountId, nameSpace, serverId).view())) {
                return Entity::ETS::TransferServer::fromDocument(result->view());
            }

        } catch (const std::exception &e) {
            log_error << "Find transfer server failed, error: " << e.what();
        }
        return std::nullopt;
    }

    std::optional<Entity::ETS::TransferServer> MongoEtsRepository::findServerByErn(const std::string &ern) const {

        try {
            auto collection = Database::instance().collection(COLLECTION);

            if (const auto result = collection.find_one(make_document(kvp("ern", ern)).view())) {
                return Entity::ETS::TransferServer::fromDocument(result->view());
            }

        } catch (const std::exception &e) {
            log_error << "Find transfer server by ERN failed, error: " << e.what();
        }
        return std::nullopt;
    }

    std::optional<Entity::ETS::TransferServer> MongoEtsRepository::findServerByRuntimeName(const std::string &runtimeName) const {

        try {
            auto collection = Database::instance().collection(COLLECTION);

            // Either a server issued that name, or one from before the field existed is running
            // under it by virtue of being called that - RuntimeName() answers the bare serverId
            // for those, so a name already taken that way is just as taken.
            const auto filter = make_document(kvp("$or", make_array(
                    make_document(kvp("runtimeName", runtimeName)),
                    make_document(kvp("serverId", runtimeName), kvp("runtimeName", make_document(kvp("$exists", false)))))));

            if (const auto result = collection.find_one(filter.view())) {
                return Entity::ETS::TransferServer::fromDocument(result->view());
            }

        } catch (const std::exception &e) {
            log_error << "Find transfer server by runtime name failed, error: " << e.what();
        }
        return std::nullopt;
    }

    bool MongoEtsRepository::serverExists(const std::string &accountId, const std::string &nameSpace,
                                          const std::string &serverId) const {
        return findServerByServerId(accountId, nameSpace, serverId).has_value();
    }

    std::vector<Entity::ETS::TransferServer> MongoEtsRepository::listServers(const std::string &accountId, const std::string &nameSpace,
                                                                            const std::string &prefix) const {

        try {
            bsoncxx::builder::basic::document filter{};
            filter.append(kvp("accountId", accountId), kvp("namespace", nameSpace));
            if (!prefix.empty()) {
                filter.append(kvp("serverId", make_document(kvp("$regex", "^" + prefix))));
            }

            mongocxx::options::find opts;
            opts.sort(make_document(kvp("serverId", 1)));

            auto collection = Database::instance().collection(COLLECTION);

            std::vector<Entity::ETS::TransferServer> result;
            for (auto cursor = collection.find(filter.extract(), opts); const auto &doc: cursor) {
                result.push_back(Entity::ETS::TransferServer::fromDocument(doc));
            }
            return result;

        } catch (const std::exception &e) {
            log_error << "List transfer servers failed, error: " << e.what();
            return {};
        }
    }

    std::vector<Entity::ETS::TransferServer> MongoEtsRepository::listAllServers(const std::string &prefix) const {

        try {
            bsoncxx::builder::basic::document filter{};
            if (!prefix.empty()) {
                filter.append(kvp("serverId", make_document(kvp("$regex", "^" + prefix))));
            }

            mongocxx::options::find opts;
            opts.sort(make_document(kvp("serverId", 1)));

            auto collection = Database::instance().collection(COLLECTION);

            std::vector<Entity::ETS::TransferServer> result;
            for (auto cursor = collection.find(filter.extract(), opts); const auto &doc: cursor) {
                result.push_back(Entity::ETS::TransferServer::fromDocument(doc));
            }
            return result;

        } catch (const std::exception &e) {
            log_error << "List transfer servers failed, error: " << e.what();
            return {};
        }
    }

    long MongoEtsRepository::countServers(const std::string &accountId, const std::string &nameSpace) const {

        try {
            auto collection = Database::instance().collection(COLLECTION);
            return static_cast<long>(collection.count_documents(make_document(kvp("accountId", accountId), kvp("namespace", nameSpace)).view()));

        } catch (const std::exception &e) {
            log_error << "Count transfer servers failed, error: " << e.what();
            return 0;
        }
    }

    void MongoEtsRepository::deleteServer(const std::string &accountId, const std::string &nameSpace,
                                         const std::string &serverId) {

        try {
            auto collection = Database::instance().collection(COLLECTION);

            const auto result = collection.delete_one(serverFilter(accountId, nameSpace, serverId).view());
            log_debug << "Transfer server deleted, serverId: " << serverId << ", count: " << (result ? result->deleted_count() : 0);

        } catch (const std::exception &e) {
            log_error << "Delete transfer server failed, error: " << e.what();
        }
    }

    void MongoEtsRepository::clear() {

        try {
            auto collection = Database::instance().collection(COLLECTION);

            const auto result = collection.delete_many({});
            log_debug << "Transfer servers cleared, count: " << (result ? result->deleted_count() : 0);

        } catch (const std::exception &e) {
            log_error << "Clear transfer servers failed, error: " << e.what();
        }
    }

}// namespace Euclid::Database
