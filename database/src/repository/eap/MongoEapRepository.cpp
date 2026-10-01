// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/1/26.
//

#include <bsoncxx/builder/basic/array.hpp>
#include <euclid/database/repository/eap/MongoEapRepository.h>

namespace Euclid::Database {

    MongoEapRepository::MongoEapRepository() {
        ensureIndexes();
    }

    void MongoEapRepository::ensureIndexes() {

        try {
            auto collection = Database::instance().collection(COLLECTION);

            // Compound on (accountId, namespace, applicationId) rather than applicationId alone -
            // an application is named within its account and namespace, like every other resource.
            // What has to stay unique installation-wide is the name it *runs* under, which is a
            // name of its own and has an index of its own below.
            //
            // NOTE: replacing a pre-existing unique index on "applicationId" alone requires
            // dropping that old index first (db.eap_application.dropIndex("applicationId_1")) -
            // Mongo won't do this automatically.
            mongocxx::options::index applicationIdOpts;
            applicationIdOpts.unique(true);
            collection.create_index(make_document(kvp("accountId", 1), kvp("namespace", 1), kvp("applicationId", 1)), applicationIdOpts);

            mongocxx::options::index ernOpts;
            ernOpts.unique(true);
            collection.create_index(make_document(kvp("ern", 1)), ernOpts);

            // What the manager, the module registry and the filesystem know an application by, and
            // the one name that has to be unique across the whole installation - a process pool, a
            // data directory and a unix socket have no account or namespace to be unique within.
            //
            // Sparse, because an application deployed before the field existed has none: it runs
            // under its bare applicationId, which the compound index above already keeps unique
            // within its account and namespace. A sparse index skips those documents rather than
            // reading them all as one shared empty name.
            mongocxx::options::index runtimeNameOpts;
            runtimeNameOpts.unique(true);
            runtimeNameOpts.sparse(true);
            collection.create_index(make_document(kvp("runtimeName", 1)), runtimeNameOpts);

        } catch (const std::exception &e) {
            log_error << "Ensure application indexes failed, error: " << e.what();
        }
    }

    bsoncxx::document::value MongoEapRepository::applicationFilter(const std::string &accountId, const std::string &nameSpace,
                                                                   const std::string &applicationId) {
        // The three fields the unique index is built on, in its order.
        return make_document(kvp("accountId", accountId), kvp("namespace", nameSpace), kvp("applicationId", applicationId));
    }

    Entity::EAP::Application MongoEapRepository::upsertApplication(Entity::EAP::Application &application) {

        try {
            auto collection = Database::instance().collection(COLLECTION);

            // created is carried in toDocument(), so it is stamped here on the insert path rather
            // than through $setOnInsert - which would collide with the same field in $set.
            const auto now = std::chrono::system_clock::now();
            if (application.created.time_since_epoch().count() == 0) {
                application.created = now;
            }
            application.modified = now;

            mongocxx::options::find_one_and_update opts;
            opts.upsert(true);
            opts.return_document(mongocxx::options::return_document::k_after);

            // Matches the unique index: filtered on the applicationId alone this would find another
            // account's application of that name and overwrite its definition.
            const auto filter = applicationFilter(application.accountId, application.nameSpace, application.applicationId);
            if (auto result = collection.find_one_and_update(filter.view(),
                                                             make_document(kvp("$set", application.toDocument())).view(), opts)) {
                return Entity::EAP::Application::fromDocument(result->view());
            }

        } catch (const std::exception &e) {
            log_error << "Upsert application failed, error: " << e.what();
        }
        return application;
    }

    std::optional<Entity::EAP::Application> MongoEapRepository::findApplicationByApplicationId(const std::string &accountId, const std::string &nameSpace,
                                                                                               const std::string &applicationId) const {

        try {
            auto collection = Database::instance().collection(COLLECTION);

            if (const auto result = collection.find_one(applicationFilter(accountId, nameSpace, applicationId).view())) {
                return Entity::EAP::Application::fromDocument(result->view());
            }

        } catch (const std::exception &e) {
            log_error << "Find application failed, error: " << e.what();
        }
        return std::nullopt;
    }

    std::optional<Entity::EAP::Application> MongoEapRepository::findApplicationByErn(const std::string &ern) const {

        try {
            auto collection = Database::instance().collection(COLLECTION);

            if (const auto result = collection.find_one(make_document(kvp("ern", ern)).view())) {
                return Entity::EAP::Application::fromDocument(result->view());
            }

        } catch (const std::exception &e) {
            log_error << "Find application by ERN failed, error: " << e.what();
        }
        return std::nullopt;
    }

    std::optional<Entity::EAP::Application> MongoEapRepository::findApplicationByRuntimeName(const std::string &runtimeName) const {

        try {
            auto collection = Database::instance().collection(COLLECTION);

            // Either an application issued that name, or one from before the field existed is
            // running under it by virtue of being called that - RuntimeName() answers the bare
            // applicationId for those, so a name already taken that way is just as taken.
            const auto filter = make_document(kvp("$or", make_array(
                    make_document(kvp("runtimeName", runtimeName)),
                    make_document(kvp("applicationId", runtimeName), kvp("runtimeName", make_document(kvp("$exists", false)))))));

            if (const auto result = collection.find_one(filter.view())) {
                return Entity::EAP::Application::fromDocument(result->view());
            }

        } catch (const std::exception &e) {
            log_error << "Find application by runtime name failed, error: " << e.what();
        }
        return std::nullopt;
    }

    bool MongoEapRepository::applicationExists(const std::string &accountId, const std::string &nameSpace,
                                               const std::string &applicationId) const {
        return findApplicationByApplicationId(accountId, nameSpace, applicationId).has_value();
    }

    std::vector<Entity::EAP::Application> MongoEapRepository::listApplications(const std::string &accountId, const std::string &nameSpace,
                                                                               const std::string &prefix) const {

        try {
            bsoncxx::builder::basic::document filter{};
            filter.append(kvp("accountId", accountId), kvp("namespace", nameSpace));
            if (!prefix.empty()) {
                filter.append(kvp("applicationId", make_document(kvp("$regex", "^" + prefix))));
            }

            mongocxx::options::find opts;
            opts.sort(make_document(kvp("applicationId", 1)));

            auto collection = Database::instance().collection(COLLECTION);

            std::vector<Entity::EAP::Application> result;
            for (auto cursor = collection.find(filter.extract(), opts); const auto &doc: cursor) {
                result.push_back(Entity::EAP::Application::fromDocument(doc));
            }
            return result;

        } catch (const std::exception &e) {
            log_error << "List applications failed, error: " << e.what();
            return {};
        }
    }

    std::vector<Entity::EAP::Application> MongoEapRepository::listAllApplications(const std::string &prefix) const {

        try {
            bsoncxx::builder::basic::document filter{};
            if (!prefix.empty()) {
                filter.append(kvp("applicationId", make_document(kvp("$regex", "^" + prefix))));
            }

            mongocxx::options::find opts;
            opts.sort(make_document(kvp("applicationId", 1)));

            auto collection = Database::instance().collection(COLLECTION);

            std::vector<Entity::EAP::Application> result;
            for (auto cursor = collection.find(filter.extract(), opts); const auto &doc: cursor) {
                result.push_back(Entity::EAP::Application::fromDocument(doc));
            }
            return result;

        } catch (const std::exception &e) {
            log_error << "List applications failed, error: " << e.what();
            return {};
        }
    }

    long MongoEapRepository::countApplications(const std::string &accountId, const std::string &nameSpace) const {

        try {
            auto collection = Database::instance().collection(COLLECTION);
            return static_cast<long>(collection.count_documents(make_document(kvp("accountId", accountId), kvp("namespace", nameSpace)).view()));

        } catch (const std::exception &e) {
            log_error << "Count applications failed, error: " << e.what();
            return 0;
        }
    }

    void MongoEapRepository::deleteApplication(const std::string &accountId, const std::string &nameSpace,
                                               const std::string &applicationId) {

        try {
            auto collection = Database::instance().collection(COLLECTION);
            collection.delete_one(applicationFilter(accountId, nameSpace, applicationId).view());

        } catch (const std::exception &e) {
            log_error << "Delete application failed, error: " << e.what();
        }
    }

    bool MongoEapRepository::setApplicationInstances(const std::string &accountId, const std::string &nameSpace,
                                                     const std::string &applicationId, const long minInstances,
                                                     const long maxInstances) {

        try {
            // Nothing asked for is not an error, but it must not become an empty $set - MongoDB
            // rejects that, and "leave both alone" is a request that is already satisfied.
            if (minInstances < 0 && maxInstances < 0) return true;

            bsoncxx::builder::basic::document fields;
            if (minInstances >= 0) fields.append(kvp("minInstances", static_cast<std::int64_t>(minInstances)));
            if (maxInstances >= 0) fields.append(kvp("maxInstances", static_cast<std::int64_t>(maxInstances)));

            auto collection = Database::instance().collection(COLLECTION);

            // The two fields by hand rather than through upsertApplication(), which stamps
            // "modified" - the revision the manager restarts a pool on. See the interface.
            const auto result = collection.update_one(applicationFilter(accountId, nameSpace, applicationId).view(),
                                                      make_document(kvp("$set", fields.extract())).view());
            return result && result->matched_count() > 0;

        } catch (const std::exception &e) {
            log_error << "Set application instances failed, applicationId: " << applicationId << ", error: " << e.what();
        }
        return false;
    }

    bool MongoEapRepository::setApplicationInfrastructure(const std::string &accountId, const std::string &nameSpace,
                                                          const std::string &applicationId,
                                                          const std::vector<std::string> &infrastructure) {

        try {
            bsoncxx::builder::basic::array erns;
            for (const auto &ern: infrastructure) erns.append(ern);

            auto collection = Database::instance().collection(COLLECTION);

            // One field, by hand, for the reason setApplicationLogLevel gives below: upsertApplication
            // stamps "modified", and the manager restarts a pool whose application has been modified
            // since its instances started. Applying a declaration touches no process.
            const auto result = collection.update_one(applicationFilter(accountId, nameSpace, applicationId).view(),
                                                      make_document(kvp("$set", make_document(kvp("infrastructure", erns)))).view());
            return result && result->matched_count() > 0;

        } catch (const std::exception &e) {
            log_error << "Set application infrastructure failed, applicationId: " << applicationId << ", error: " << e.what();
        }
        return false;
    }

    bool MongoEapRepository::setApplicationLogLevel(const std::string &accountId, const std::string &nameSpace,
                                                    const std::string &applicationId, const std::string &logLevel) {

        try {
            auto collection = Database::instance().collection(COLLECTION);

            // One field, by hand, rather than through upsertApplication(): that writes the whole
            // document and stamps "modified", which the manager compares against the revision the
            // running instances were started with - so saving a log level through it would restart
            // the application.
            const auto result = collection.update_one(applicationFilter(accountId, nameSpace, applicationId).view(),
                                                      make_document(kvp("$set", make_document(kvp("logLevel", logLevel)))).view());
            return result && result->matched_count() > 0;

        } catch (const std::exception &e) {
            log_error << "Set application log level failed, applicationId: " << applicationId << ", error: " << e.what();
        }
        return false;
    }

    bool MongoEapRepository::touchApplication(const std::string &accountId, const std::string &nameSpace,
                                              const std::string &applicationId) {

        try {
            auto collection = Database::instance().collection(COLLECTION);

            // $currentDate rather than a time read here: the manager compares this against the
            // revision it started an instance with, so the two have to be stamped by the same
            // clock as every other write to this document - the database's.
            //
            // One field, for the reason setApplicationLogLevel() gives above: upsertApplication()
            // would write the whole document from a copy read moments ago, so a restart would
            // quietly revert anything changed in between.
            const auto result = collection.update_one(applicationFilter(accountId, nameSpace, applicationId).view(),
                                                      make_document(kvp("$currentDate", make_document(kvp("modified", true)))).view());
            return result && result->matched_count() > 0;

        } catch (const std::exception &e) {
            log_error << "Touch application failed, applicationId: " << applicationId << ", error: " << e.what();
        }
        return false;
    }

    // ── Worker nodes ────────────────────────────────────────────────────────────────────────────

    bsoncxx::document::value MongoEapRepository::nodeFilter(const std::string &accountId, const std::string &name) {
        return make_document(kvp("accountId", accountId), kvp("name", name));
    }

    Entity::EAP::Node MongoEapRepository::upsertNode(Entity::EAP::Node &node) {

        try {
            auto collection = Database::instance().collection(NODE_COLLECTION);

            node.modified = std::chrono::system_clock::now();

            mongocxx::options::find_one_and_update opts;
            opts.return_document(mongocxx::options::return_document::k_after);
            opts.upsert(true);

            // The document whole, which is right here and wrong for the two targeted writes below:
            // registration is the worker stating everything about itself, so a label it no longer
            // has should go. `created` is kept on insert only, so re-registering does not make a
            // node look new.
            const auto update = make_document(
                    kvp("$set", node.toDocument()),
                    kvp("$setOnInsert", make_document(kvp("created", bsoncxx::types::b_date{
                                                                             std::chrono::duration_cast<std::chrono::milliseconds>(node.created.time_since_epoch())}))));

            if (const auto result = collection.find_one_and_update(nodeFilter(node.accountId, node.name).view(), update.view(), opts)) {
                return Entity::EAP::Node::fromDocument(result->view());
            }

        } catch (const std::exception &e) {
            log_error << "Upsert node failed, node: " << node.name << ", error: " << e.what();
        }
        return node;
    }

    std::optional<Entity::EAP::Node> MongoEapRepository::findNodeByName(const std::string &accountId, const std::string &name) const {

        try {
            auto collection = Database::instance().collection(NODE_COLLECTION);
            if (const auto result = collection.find_one(nodeFilter(accountId, name).view())) {
                return Entity::EAP::Node::fromDocument(result->view());
            }
        } catch (const std::exception &e) {
            log_error << "Find node failed, node: " << name << ", error: " << e.what();
        }
        return std::nullopt;
    }

    std::vector<Entity::EAP::Node> MongoEapRepository::listNodes(const std::string &accountId) const {

        std::vector<Entity::EAP::Node> nodes;
        try {
            auto collection = Database::instance().collection(NODE_COLLECTION);
            for (auto cursor = collection.find(make_document(kvp("accountId", accountId)).view()); const auto &document: cursor) {
                nodes.push_back(Entity::EAP::Node::fromDocument(document));
            }
        } catch (const std::exception &e) {
            log_error << "List nodes failed, accountId: " << accountId << ", error: " << e.what();
        }
        return nodes;
    }

    bool MongoEapRepository::touchNode(const std::string &accountId, const std::string &name,
                                       const std::chrono::system_clock::time_point seenAt) {

        try {
            auto collection = Database::instance().collection(NODE_COLLECTION);

            // Two fields, not the document: this runs on every tick of every worker, and a
            // read-modify-write would race an operator draining the node at the same moment and
            // put `drained` back to whatever the worker last registered with.
            const auto update = make_document(
                    kvp("$set", make_document(kvp("lastSeen", bsoncxx::types::b_date{
                                                                      std::chrono::duration_cast<std::chrono::milliseconds>(seenAt.time_since_epoch())}))),
                    kvp("$currentDate", make_document(kvp("modified", true))));

            const auto result = collection.update_one(nodeFilter(accountId, name).view(), update.view());
            return result && result->matched_count() > 0;

        } catch (const std::exception &e) {
            log_error << "Touch node failed, node: " << name << ", error: " << e.what();
        }
        return false;
    }

    bool MongoEapRepository::setNodeDrained(const std::string &accountId, const std::string &name, const bool drained) {

        try {
            auto collection = Database::instance().collection(NODE_COLLECTION);

            const auto update = make_document(
                    kvp("$set", make_document(kvp("drained", drained))),
                    kvp("$currentDate", make_document(kvp("modified", true))));

            const auto result = collection.update_one(nodeFilter(accountId, name).view(), update.view());
            return result && result->matched_count() > 0;

        } catch (const std::exception &e) {
            log_error << "Set node drained failed, node: " << name << ", error: " << e.what();
        }
        return false;
    }

    bool MongoEapRepository::deleteNode(const std::string &accountId, const std::string &name) {

        try {
            auto collection = Database::instance().collection(NODE_COLLECTION);
            const auto result = collection.delete_one(nodeFilter(accountId, name).view());
            return result && result->deleted_count() > 0;

        } catch (const std::exception &e) {
            log_error << "Delete node failed, node: " << name << ", error: " << e.what();
        }
        return false;
    }

}// namespace Euclid::Database
