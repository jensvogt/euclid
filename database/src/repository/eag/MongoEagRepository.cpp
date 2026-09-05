//
// Created by vogje01 on 9/5/26.
//

#include <euclid/database/repository/eag/MongoEagRepository.h>

namespace Euclid::Database {

    // In the source rather than the header. Several of the other Mongo repositories declare this
    // in their header and the rest compile only because one of those is pulled in transitively -
    // a dependency nobody wrote down and any include-order change would break.
    using bsoncxx::builder::basic::kvp;
    using bsoncxx::builder::basic::make_document;

    MongoEagRepository::MongoEagRepository() {
        ensureIndexes();
    }

    void MongoEagRepository::ensureIndexes() {

        try {
            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];

            // A duplicate routeId would give the gateway two definitions of the same resource and
            // no way to say which is meant.
            mongocxx::options::index routeIdOpts;
            routeIdOpts.unique(true);
            collection.create_index(make_document(kvp("routeId", 1)), routeIdOpts);

            mongocxx::options::index ernOpts;
            ernOpts.unique(true);
            collection.create_index(make_document(kvp("ern", 1)), ernOpts);

            // Deliberately not unique: two routes may share a path while differing in what they
            // require of a caller, and the gateway resolves that by taking the longest match.
            collection.create_index(make_document(kvp("path", 1)));

        } catch (const std::exception &e) {
            log_error << "Ensure route indexes failed, error: " << e.what();
        }
    }

    std::optional<Entity::EAG::Route> MongoEagRepository::upsertRoute(Entity::EAG::Route &route) {

        try {
            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];

            // created is carried in toDocument(), so it is stamped here on the insert path rather
            // than through $setOnInsert - which would collide with the same field in $set, and
            // Mongo rejects the whole update rather than the one field.
            const auto now = std::chrono::system_clock::now();
            if (route.created.time_since_epoch().count() == 0) {
                route.created = now;
            }
            route.modified = now;

            mongocxx::options::find_one_and_update opts;
            opts.upsert(true);
            opts.return_document(mongocxx::options::return_document::k_after);

            if (auto result = collection.find_one_and_update(make_document(kvp("routeId", route.routeId)).view(),
                                                             make_document(kvp("$set", route.toDocument())).view(), opts)) {
                return Entity::EAG::Route::fromDocument(result->view());
            }

            // Reached only if the upsert neither matched nor inserted, which should not happen -
            // said out loud rather than returning the unsaved route as though it had been stored.
            log_error << "Upsert route stored nothing, routeId: " << route.routeId;

        } catch (const std::exception &e) {
            log_error << "Upsert route failed, routeId: " << route.routeId << ", error: " << e.what();
        }
        return std::nullopt;
    }

    std::optional<Entity::EAG::Route> MongoEagRepository::findRouteByRouteId(const std::string &routeId) const {

        try {
            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];
            if (const auto result = collection.find_one(make_document(kvp("routeId", routeId)))) {
                return Entity::EAG::Route::fromDocument(result->view());
            }
        } catch (const std::exception &e) {
            log_error << "Find route failed, routeId: " << routeId << ", error: " << e.what();
        }
        return std::nullopt;
    }

    std::optional<Entity::EAG::Route> MongoEagRepository::findRouteByErn(const std::string &ern) const {

        try {
            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];
            if (const auto result = collection.find_one(make_document(kvp("ern", ern)))) {
                return Entity::EAG::Route::fromDocument(result->view());
            }
        } catch (const std::exception &e) {
            log_error << "Find route failed, ern: " << ern << ", error: " << e.what();
        }
        return std::nullopt;
    }

    std::vector<Entity::EAG::Route> MongoEagRepository::listRoutes(const std::string &prefix) const {

        std::vector<Entity::EAG::Route> routes;
        try {
            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];

            // Escaped, because a path is not a pattern: "/api/v1.0" would otherwise match
            // "/api/v1X0" as well, and a caller has no reason to expect their path to be read as
            // a regular expression.
            std::string quoted;
            for (const char c: prefix) {
                if (std::string_view(R"(\^$.|?*+()[]{})").contains(c)) quoted += '\\';
                quoted += c;
            }

            bsoncxx::builder::basic::document filter;
            if (!prefix.empty()) filter.append(kvp("path", make_document(kvp("$regex", "^" + quoted))));

            for (auto cursor = collection.find(filter.view()); auto doc: cursor) {
                routes.push_back(Entity::EAG::Route::fromDocument(doc));
            }
        } catch (const std::exception &e) {
            log_error << "List routes failed, error: " << e.what();
        }
        return routes;
    }

    bool MongoEagRepository::routeExists(const std::string &routeId) const {

        try {
            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];
            return collection.count_documents(make_document(kvp("routeId", routeId))) > 0;
        } catch (const std::exception &e) {
            log_error << "Route exists failed, routeId: " << routeId << ", error: " << e.what();
        }
        return false;
    }

    void MongoEagRepository::deleteRoute(const std::string &routeId) {

        try {
            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];
            const auto result = collection.delete_many(make_document(kvp("routeId", routeId)));
            log_debug << "Route deleted, routeId: " << routeId << ", count: " << (result ? result->deleted_count() : 0);
        } catch (const std::exception &e) {
            log_error << "Delete route failed, routeId: " << routeId << ", error: " << e.what();
        }
    }

    long MongoEagRepository::countRoutes() const {

        try {
            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];
            return static_cast<long>(collection.count_documents({}));
        } catch (const std::exception &e) {
            log_error << "Count routes failed, error: " << e.what();
        }
        return 0;
    }

    void MongoEagRepository::clear() {

        try {
            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];
            const auto result = collection.delete_many({});
            log_debug << "Routes deleted, count: " << (result ? result->deleted_count() : 0);
        } catch (const std::exception &e) {
            log_error << "Clear routes failed, error: " << e.what();
        }
    }

}// namespace Euclid::Database
