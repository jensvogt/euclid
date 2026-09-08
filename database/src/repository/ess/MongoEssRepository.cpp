//
// Created by vogje01 on 9/8/26.
//

// C++ includes
#include <chrono>

// Euclid includes
#include <euclid/database/repository/ess/MongoEssRepository.h>

namespace Euclid::Database {

    MongoEssRepository::MongoEssRepository() {
        ensureIndexes();
    }

    void MongoEssRepository::ensureIndexes() {

        try {
            const auto collection = Database::instance().collection(SECRET_COLLECTION);

            // Compound on (accountId, namespace, name), like every other named resource: a secret
            // name only has to be unique within the account and namespace that owns it, and two
            // environments naming their database password the same thing is the ordinary case
            // rather than a collision.
            mongocxx::options::index nameOpts;
            nameOpts.unique(true);
            collection.create_index(make_document(kvp("accountId", 1), kvp("namespace", 1), kvp("name", 1)), nameOpts);

            mongocxx::options::index ernOpts;
            ernOpts.unique(true);
            collection.create_index(make_document(kvp("ern", 1)), ernOpts);

        } catch (const std::exception &e) {
            log_error << "Ensure ESS indexes failed, error: " << e.what();
        }
    }

    Entity::ESS::Secret MongoEssRepository::upsertSecret(Entity::ESS::Secret &secret) {

        try {

            const auto filter = make_document(kvp("accountId", secret.accountId), kvp("namespace", secret.nameSpace), kvp("name", secret.name));
            const auto update = make_document(
                    kvp("$set", secret.toDocument()),
                    kvp("$setOnInsert", make_document(
                                                kvp("created", bsoncxx::types::b_date{
                                                                       std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                               secret.created.time_since_epoch())}))),
                    kvp("$currentDate", make_document(kvp("modified", true))));

            mongocxx::options::find_one_and_update opts;
            opts.upsert(true);
            opts.return_document(mongocxx::options::return_document::k_after);

            const auto collection = Database::instance().collection(SECRET_COLLECTION);

            if (auto result = collection.find_one_and_update(filter.view(), update.view(), opts)) {
                return Entity::ESS::Secret::fromDocument(result->view());
            }
            throw std::runtime_error("upsert returned no document, name: " + secret.name);

        } catch (const std::exception &e) {
            // Without the value, and without anything derived from it: this is the one log line in
            // the module that runs with a secret in scope.
            log_error << "Upsert secret failed, name: " << secret.name << ", error: " << e.what();
            throw;
        }
    }

    std::optional<Entity::ESS::Secret> MongoEssRepository::findSecretByName(const std::string &accountId, const std::string &namespaceName, const std::string &name) const {

        try {

            const auto collection = Database::instance().collection(SECRET_COLLECTION);

            const auto filter = make_document(kvp("accountId", accountId), kvp("namespace", namespaceName), kvp("name", name));
            if (auto result = collection.find_one(filter.view())) {
                return Entity::ESS::Secret::fromDocument(result->view());
            }

        } catch (const std::exception &e) {
            log_error << "Find secret by name failed, name: " << name << ", error: " << e.what();
        }
        return std::nullopt;
    }

    std::optional<Entity::ESS::Secret> MongoEssRepository::findSecretByErn(const std::string &ern) const {

        try {

            const auto collection = Database::instance().collection(SECRET_COLLECTION);

            if (auto result = collection.find_one(make_document(kvp("ern", ern)).view())) {
                return Entity::ESS::Secret::fromDocument(result->view());
            }

        } catch (const std::exception &e) {
            log_error << "Find secret by ERN failed, ern: " << ern << ", error: " << e.what();
        }
        return std::nullopt;
    }

    bool MongoEssRepository::secretExists(const std::string &accountId, const std::string &namespaceName, const std::string &name) const {

        try {

            const auto collection = Database::instance().collection(SECRET_COLLECTION);

            const auto filter = make_document(kvp("accountId", accountId), kvp("namespace", namespaceName), kvp("name", name));
            return collection.find_one(filter.view()).has_value();

        } catch (const std::exception &e) {
            log_error << "Secret exists failed, name: " << name << ", error: " << e.what();
        }
        return false;
    }

    std::vector<Entity::ESS::Secret> MongoEssRepository::listSecrets(const std::string &accountId, const std::string &namespaceName, const std::string &prefix, const long pageSize, const long pageIndex, const std::string &sortColumn, const std::string &sortDirection) const {

        try {

            document filter = {};
            filter.append(kvp("accountId", accountId));
            if (!namespaceName.empty()) {
                filter.append(kvp("namespace", namespaceName));
            }
            if (!prefix.empty()) {
                filter.append(kvp("name", make_document(kvp("$regex", "^" + prefix))));
            }

            mongocxx::options::find opts;
            if (!sortColumn.empty()) {
                opts.sort(make_document(kvp(sortColumn, sortDirection == "asc" ? 1 : -1)));
            }
            if (pageSize > 0) {
                opts.limit(pageSize);
                opts.skip(std::max<long>(pageIndex, 0) * pageSize);
            }

            std::vector<Entity::ESS::Secret> secrets;
            const auto collection = Database::instance().collection(SECRET_COLLECTION);

            for (auto cursor = collection.find(filter.view(), opts); auto secret: cursor) {
                secrets.push_back(Entity::ESS::Secret::fromDocument(secret));
            }
            return secrets;

        } catch (const std::exception &e) {
            log_error << "List secrets failed, error: " << e.what();
            return {};
        }
    }

    long MongoEssRepository::countSecrets(const std::string &accountId, const std::string &namespaceName, const std::string &prefix) const {

        try {

            document filter = {};
            filter.append(kvp("accountId", accountId));
            if (!namespaceName.empty()) {
                filter.append(kvp("namespace", namespaceName));
            }
            if (!prefix.empty()) {
                filter.append(kvp("name", make_document(kvp("$regex", "^" + prefix))));
            }

            const auto collection = Database::instance().collection(SECRET_COLLECTION);

            return static_cast<long>(collection.count_documents(filter.extract()));

        } catch (const std::exception &e) {
            log_error << "Secret count failed, error: " << e.what();
        }
        return -1;
    }

    long MongoEssRepository::deleteSecret(const std::string &accountId, const std::string &namespaceName, const std::string &name) {

        try {

            const auto collection = Database::instance().collection(SECRET_COLLECTION);

            const auto filter = make_document(kvp("accountId", accountId), kvp("namespace", namespaceName), kvp("name", name));
            const auto result = collection.delete_many(filter.view());
            const auto count = result ? result->deleted_count() : 0;
            log_debug << "Secret deleted, name: " << name << ", count: " << count;
            return static_cast<long>(count);

        } catch (const std::exception &e) {
            log_error << "Delete secret failed, name: " << name << ", error: " << e.what();
        }
        return 0;
    }

}// namespace Euclid::Database
