// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/5/26.
//

#include <bsoncxx/builder/concatenate.hpp>
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
            auto collection = Database::instance().collection(COLLECTION);

            // Compound on (accountId, namespace, routeId) rather than routeId alone - a route is
            // named within its account and namespace, like every other resource.
            //
            // What still has to be unique more widely is the path a route claims, and that is not
            // this index's business: a listener is bound to a namespace and answers on a port of
            // its own, so "which route serves this path" is settled per namespace rather than per
            // account - see EagServer's pathTaken(), which enforces exactly that.
            //
            // NOTE: replacing a pre-existing unique index on "routeId" alone requires dropping that
            // old index first (db.eag_route.dropIndex("routeId_1")) - Mongo won't do it itself.
            mongocxx::options::index routeIdOpts;
            routeIdOpts.unique(true);
            collection.create_index(make_document(kvp("accountId", 1), kvp("namespace", 1), kvp("routeId", 1)), routeIdOpts);

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

    bsoncxx::document::value MongoEagRepository::routeFilter(const std::string &accountId, const std::string &nameSpace,
                                                             const std::string &routeId) {
        // The three fields the unique index is built on, in its order.
        return make_document(kvp("accountId", accountId), kvp("namespace", nameSpace), kvp("routeId", routeId));
    }

    std::optional<Entity::EAG::Route> MongoEagRepository::upsertRoute(Entity::EAG::Route &route) {

        try {
            auto collection = Database::instance().collection(COLLECTION);

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

            // Matches the unique index: filtered on the routeId alone this would find another
            // account's route of that name and overwrite where it points.
            const auto filter = routeFilter(route.accountId, route.nameSpace, route.routeId);
            if (auto result = collection.find_one_and_update(filter.view(),
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

    std::optional<Entity::EAG::Route> MongoEagRepository::findRouteByRouteId(const std::string &accountId, const std::string &nameSpace,
                                                                             const std::string &routeId) const {

        try {
            auto collection = Database::instance().collection(COLLECTION);
            if (const auto result = collection.find_one(routeFilter(accountId, nameSpace, routeId).view())) {
                return Entity::EAG::Route::fromDocument(result->view());
            }
        } catch (const std::exception &e) {
            log_error << "Find route failed, routeId: " << routeId << ", error: " << e.what();
        }
        return std::nullopt;
    }

    std::optional<Entity::EAG::Route> MongoEagRepository::findRouteByErn(const std::string &ern) const {

        try {
            auto collection = Database::instance().collection(COLLECTION);
            if (const auto result = collection.find_one(make_document(kvp("ern", ern)))) {
                return Entity::EAG::Route::fromDocument(result->view());
            }
        } catch (const std::exception &e) {
            log_error << "Find route failed, ern: " << ern << ", error: " << e.what();
        }
        return std::nullopt;
    }

    std::vector<Entity::EAG::Route> MongoEagRepository::listRoutes(const std::string &accountId, const std::string &nameSpace,
                                                                   const std::string &prefix) const {
        return findRoutes(prefix, make_document(kvp("accountId", accountId), kvp("namespace", nameSpace)).view());
    }

    std::vector<Entity::EAG::Route> MongoEagRepository::listAllRoutes(const std::string &prefix) const {
        return findRoutes(prefix, {});
    }

    std::vector<Entity::EAG::Route> MongoEagRepository::findRoutes(const std::string &prefix,
                                                                   const bsoncxx::document::view &scope) {

        std::vector<Entity::EAG::Route> routes;
        try {
            auto collection = Database::instance().collection(COLLECTION);

            // Escaped, because a path is not a pattern: "/api/v1.0" would otherwise match
            // "/api/v1X0" as well, and a caller has no reason to expect their path to be read as
            // a regular expression.
            std::string quoted;
            for (const char c: prefix) {
                if (std::string_view(R"(\^$.|?*+()[]{})").contains(c)) quoted += '\\';
                quoted += c;
            }

            bsoncxx::builder::basic::document filter;
            filter.append(bsoncxx::builder::concatenate(scope));
            if (!prefix.empty()) filter.append(kvp("path", make_document(kvp("$regex", "^" + quoted))));

            for (auto cursor = collection.find(filter.view()); auto doc: cursor) {
                routes.push_back(Entity::EAG::Route::fromDocument(doc));
            }
        } catch (const std::exception &e) {
            log_error << "List routes failed, error: " << e.what();
        }
        return routes;
    }

    bool MongoEagRepository::routeExists(const std::string &accountId, const std::string &nameSpace,
                                         const std::string &routeId) const {

        try {
            auto collection = Database::instance().collection(COLLECTION);
            return collection.count_documents(routeFilter(accountId, nameSpace, routeId).view()) > 0;
        } catch (const std::exception &e) {
            log_error << "Route exists failed, routeId: " << routeId << ", error: " << e.what();
        }
        return false;
    }

    void MongoEagRepository::deleteRoute(const std::string &accountId, const std::string &nameSpace,
                                        const std::string &routeId) {

        try {
            auto collection = Database::instance().collection(COLLECTION);
            const auto result = collection.delete_many(routeFilter(accountId, nameSpace, routeId).view());
            log_debug << "Route deleted, routeId: " << routeId << ", count: " << (result ? result->deleted_count() : 0);
        } catch (const std::exception &e) {
            log_error << "Delete route failed, routeId: " << routeId << ", error: " << e.what();
        }
    }

    long MongoEagRepository::countRoutes(const std::string &accountId, const std::string &nameSpace) const {

        try {
            auto collection = Database::instance().collection(COLLECTION);
            return static_cast<long>(collection.count_documents(make_document(kvp("accountId", accountId), kvp("namespace", nameSpace)).view()));
        } catch (const std::exception &e) {
            log_error << "Count routes failed, error: " << e.what();
        }
        return 0;
    }

    void MongoEagRepository::clear() {

        try {
            auto collection = Database::instance().collection(COLLECTION);
            const auto result = collection.delete_many({});
            log_debug << "Routes deleted, count: " << (result ? result->deleted_count() : 0);
        } catch (const std::exception &e) {
            log_error << "Clear routes failed, error: " << e.what();
        }
    }

}// namespace Euclid::Database
