//
// Created by vogje01 on 8/31/26.
//

#include <euclid/database/repository/ets/MongoEtsRepository.h>

namespace Euclid::Database {

    MongoEtsRepository::MongoEtsRepository() {
        ensureIndexes();
    }

    void MongoEtsRepository::ensureIndexes() {

        try {
            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];

            // serverId doubles as the manager's module name for the spawned process, so a
            // duplicate would silently merge two servers into one process pool.
            mongocxx::options::index serverIdOpts;
            serverIdOpts.unique(true);
            collection.create_index(make_document(kvp("serverId", 1)), serverIdOpts);

            mongocxx::options::index ernOpts;
            ernOpts.unique(true);
            collection.create_index(make_document(kvp("ern", 1)), ernOpts);

        } catch (const std::exception &e) {
            log_error << "Ensure transfer server indexes failed, error: " << e.what();
        }
    }

    Entity::ETS::TransferServer MongoEtsRepository::upsertServer(Entity::ETS::TransferServer &server) {

        try {
            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];

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

            if (auto result = collection.find_one_and_update(make_document(kvp("serverId", server.serverId)).view(),
                                                             make_document(kvp("$set", server.toDocument())).view(), opts)) {
                return Entity::ETS::TransferServer::fromDocument(result->view());
            }

        } catch (const std::exception &e) {
            log_error << "Upsert transfer server failed, error: " << e.what();
        }
        return server;
    }

    std::optional<Entity::ETS::TransferServer> MongoEtsRepository::findServerByServerId(const std::string &serverId) const {

        try {
            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];

            if (const auto result = collection.find_one(make_document(kvp("serverId", serverId)).view())) {
                return Entity::ETS::TransferServer::fromDocument(result->view());
            }

        } catch (const std::exception &e) {
            log_error << "Find transfer server failed, error: " << e.what();
        }
        return std::nullopt;
    }

    std::optional<Entity::ETS::TransferServer> MongoEtsRepository::findServerByErn(const std::string &ern) const {

        try {
            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];

            if (const auto result = collection.find_one(make_document(kvp("ern", ern)).view())) {
                return Entity::ETS::TransferServer::fromDocument(result->view());
            }

        } catch (const std::exception &e) {
            log_error << "Find transfer server by ERN failed, error: " << e.what();
        }
        return std::nullopt;
    }

    bool MongoEtsRepository::serverExists(const std::string &serverId) const {
        return findServerByServerId(serverId).has_value();
    }

    std::vector<Entity::ETS::TransferServer> MongoEtsRepository::listServers(const std::string &prefix) const {

        try {
            bsoncxx::builder::basic::document filter{};
            if (!prefix.empty()) {
                filter.append(kvp("serverId", make_document(kvp("$regex", "^" + prefix))));
            }

            mongocxx::options::find opts;
            opts.sort(make_document(kvp("serverId", 1)));

            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];

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

    long MongoEtsRepository::countServers() const {

        try {
            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];
            return static_cast<long>(collection.count_documents({}));

        } catch (const std::exception &e) {
            log_error << "Count transfer servers failed, error: " << e.what();
            return 0;
        }
    }

    void MongoEtsRepository::deleteServer(const std::string &serverId) {

        try {
            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];

            const auto result = collection.delete_one(make_document(kvp("serverId", serverId)).view());
            log_debug << "Transfer server deleted, serverId: " << serverId << ", count: " << (result ? result->deleted_count() : 0);

        } catch (const std::exception &e) {
            log_error << "Delete transfer server failed, error: " << e.what();
        }
    }

    void MongoEtsRepository::clear() {

        try {
            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];

            const auto result = collection.delete_many({});
            log_debug << "Transfer servers cleared, count: " << (result ? result->deleted_count() : 0);

        } catch (const std::exception &e) {
            log_error << "Clear transfer servers failed, error: " << e.what();
        }
    }

}// namespace Euclid::Database
