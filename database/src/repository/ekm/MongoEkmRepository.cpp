//
// Created by vogje01 on 29/05/2023.
//

// C++ includes
#include <array>
#include <chrono>
#include <map>
#include <thread>

// Euclid includes
#include <euclid/core/CryptoUtils.h>
#include <euclid/core/UuidUtils.h>
#include <euclid/database/repository/ekm/MongoEkmRepository.h>

namespace Euclid::Database {

    MongoEkmRepository::MongoEkmRepository() {
        ensureIndexes();
    }

    void MongoEkmRepository::ensureIndexes() {

        try {
            const auto entry = Database::instance().client();
            auto keyCollection = (*entry)[Database::instance().databaseName()][KEY_COLLECTION];

            // Compound on (accountId, namespace, name) rather than name alone - queue names only
            // need to be unique within their own account/namespace, not globally. NOTE: replacing
            // a pre-existing unique index on "name" alone requires dropping that old index first
            // (Mongo won't do this automatically), and will fail if duplicate names already exist
            // across accounts in production data - this is an operational migration step.
            mongocxx::options::index queueNameOpts;
            queueNameOpts.unique(true);
            keyCollection.create_index(make_document(kvp("accountId", 1), kvp("namespace", 1), kvp("name", 1)), queueNameOpts);

            mongocxx::options::index queueErnOpts;
            queueErnOpts.unique(true);
            keyCollection.create_index(make_document(kvp("ern", 1)), queueErnOpts);

        } catch (const std::exception &e) {
            log_error << "Ensure SQS indexes failed, error: " << e.what();
        }
    }

    // bool MongoEkmRepository::queueExists(const std::string &name) const {
    //
    //     try {
    //
    //         document query{};
    //         if (!name.empty()) {
    //             query.append(kvp("name", name));
    //         }
    //
    //         const auto entry = Database::instance().client();
    //         auto keyCollection = (*entry)[Database::instance().databaseName()][QUEUE_COLLECTION];
    //
    //         const auto result = keyCollection.find_one(query.extract());
    //         log_trace << "Sqs exists, name: " << name << ", exists: " << std::boolalpha << result.has_value();
    //         return result.has_value();
    //
    //     } catch (const std::exception &e) {
    //         log_error << "Sqs exists failed, name: " << ", error: " << e.what();
    //     }
    //     return false;
    // }
    //
    // std::optional<Entity::EKM::Queue> MongoEkmRepository::findQueueById(const std::string &oid) const {
    //
    //     try {
    //
    //         document document;
    //         document.append(kvp("_id", oid));
    //
    //         const auto entry = Database::instance().client();
    //         auto keyCollection = (*entry)[Database::instance().databaseName()][QUEUE_COLLECTION];
    //
    //         if (auto mResult = keyCollection.find_one(document.view())) {
    //             return Entity::EKM::Queue::fromDocument(mResult->view());
    //         }
    //
    //     } catch (const std::exception &e) {
    //         log_error << "Get queues by ID failed, error: " << e.what();
    //     }
    //     return {};
    // }
    //
    // std::optional<Entity::EKM::Queue> MongoEkmRepository::findQueueByName(const std::string &name) const {
    //
    //     try {
    //
    //         const auto entry = Database::instance().client();
    //         auto keyCollection = (*entry)[Database::instance().databaseName()][QUEUE_COLLECTION];
    //
    //         if (auto mResult = keyCollection.find_one(make_document(kvp("name", name)))) {
    //             return Entity::EKM::Queue::fromDocument(mResult.value());
    //         }
    //
    //     } catch (const std::exception &e) {
    //         log_error << "Get queues by name failed, name: " << name << " error: " << e.what();
    //     }
    //     return {};
    // }
    //
    // std::optional<Entity::EKM::Queue> MongoEkmRepository::findQueueByErn(const std::string &ern) const {
    //
    //     try {
    //
    //         const auto entry = Database::instance().client();
    //         auto keyCollection = (*entry)[Database::instance().databaseName()][QUEUE_COLLECTION];
    //
    //         if (auto mResult = keyCollection.find_one(make_document(kvp("ern", ern)))) {
    //             return Entity::EKM::Queue::fromDocument(mResult.value());
    //         }
    //
    //     } catch (const std::exception &e) {
    //         log_error << "Get queues by ERN failed, ern: " << ern << " error: " << e.what();
    //     }
    //     return {};
    // }

    std::vector<Entity::EKM::Key> MongoEkmRepository::listKeys(const std::string &accountId, const std::string &namespaceName, const std::string &prefix, const long pageSize, const long pageIndex, const std::string &sortColumn, const std::string &sortDirection) const {

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

            std::vector<Entity::EKM::Key> keys;
            const auto entry = Database::instance().client();
            auto keyCollection = (*entry)[Database::instance().databaseName()][KEY_COLLECTION];

            for (auto queueCursor = keyCollection.find(filter.view(), opts); auto queue: queueCursor) {
                keys.push_back(Entity::EKM::Key::fromDocument(queue));
            }
            return keys;

        } catch (const std::exception &e) {

            log_error << "Get EKM queues failed, error: " << e.what();
            return {};
        }
    }

    Entity::EKM::Key MongoEkmRepository::upsertKey(Entity::EKM::Key &key) {

        try {

            const auto filter = make_document(kvp("accountId", key.accountId), kvp("namespace", key.nameSpace), kvp("name", key.name));
            const auto update = make_document(
                    kvp("$set", key.toDocument()),
                    kvp("$setOnInsert", make_document(
                                kvp("created", bsoncxx::types::b_date{
                                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                                    key.created.time_since_epoch())
                                    })
                                )),
                    kvp("$currentDate", make_document(
                                kvp("modified", true)
                                )));

            mongocxx::options::find_one_and_update opts;
            opts.upsert(true);
            opts.return_document(mongocxx::options::return_document::k_after);

            const auto entry = Database::instance().client();
            auto keyCollection = (*entry)[Database::instance().databaseName()][KEY_COLLECTION];

            if (auto result = keyCollection.find_one_and_update(filter.view(), update.view(), opts)) {
                return Entity::EKM::Key::fromDocument(result->view());
            }
            throw std::runtime_error("upsert returned no document, name: " + key.name);

        } catch (const std::exception &e) {
            log_error << "Upsert EKM queue failed, error: " << e.what();
            throw;
        }
    }

    long MongoEkmRepository::countKeys(const std::string &accountId, const std::string &nameSpace, const std::string &prefix) const {

        try {

            document filter = {};
            filter.append(kvp("accountId", accountId));
            if (!nameSpace.empty()) {
                filter.append(kvp("namespace", nameSpace));
            }
            if (!prefix.empty()) {
                filter.append(kvp("name", make_document(kvp("$regex", "^" + prefix))));
            }

            const auto entry = Database::instance().client();
            auto keyCollection = (*entry)[Database::instance().databaseName()][KEY_COLLECTION];

            const int64_t count = keyCollection.count_documents(filter.extract());
            log_trace << "Key count: " << count;
            return static_cast<int>(count);

        } catch (const std::exception &e) {
            log_error << "Key count failed, error: " << e.what();
        }
        return -1;
    }

    //
    // void MongoEkmRepository::removeQueueByName(const std::string &name) {
    //
    //     try {
    //         const auto entry = Database::instance().client();
    //         auto keyCollection = (*entry)[Database::instance().databaseName()][QUEUE_COLLECTION];
    //         auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];
    //
    //         std::vector<std::string> erns;
    //         for (auto cursor = keyCollection.find(make_document(kvp("name", name))); auto doc: cursor) {
    //             if (const auto ernField = doc["ern"]; ernField && ernField.type() == bsoncxx::type::k_string) {
    //                 erns.emplace_back(ernField.get_string().value);
    //             }
    //         }
    //
    //         const auto result = keyCollection.delete_many(make_document(kvp("name", name)));
    //         log_debug << "EKM deleted, count: " << result->deleted_count();
    //
    //         if (!erns.empty()) {
    //             array ernArray;
    //             for (const auto &ern: erns) ernArray.append(ern);
    //             const auto messageResult = messageCollection.delete_many(make_document(kvp("queueErn", make_document(kvp("$in", ernArray.view())))));
    //             log_debug << "EKM messages deleted, count: " << messageResult->deleted_count();
    //         }
    //
    //     } catch (const std::exception &e) {
    //
    //         log_error << "Delete queues failed, error: " << e.what();
    //     }
    //
    // }
    //
    // void MongoEkmRepository::deleteQueueByErn(const std::string &ern) {
    //
    //     try {
    //         const auto entry = Database::instance().client();
    //         auto keyCollection = (*entry)[Database::instance().databaseName()][QUEUE_COLLECTION];
    //         auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];
    //
    //         const auto result = keyCollection.delete_many(make_document(kvp("ern", ern)));
    //         log_debug << "EKM deleted, count: " << result->deleted_count();
    //
    //         const auto messageResult = messageCollection.delete_many(make_document(kvp("queueErn", ern)));
    //         log_debug << "EKM messages deleted, count: " << messageResult->deleted_count();
    //
    //     } catch (const std::exception &e) {
    //
    //         log_error << "Delete queues failed, error: " << e.what();
    //     }
    //
    // }
    //
    // void MongoEkmRepository::clearQueues() {
    //
    //     try {
    //         const auto entry = Database::instance().client();
    //         auto keyCollection = (*entry)[Database::instance().databaseName()][QUEUE_COLLECTION];
    //
    //         const auto result = keyCollection.delete_many({});
    //         log_debug << "All queues deleted, count: " << result->deleted_count();
    //
    //     } catch (const std::exception &e) {
    //         log_error << "Delete all queues queues failed, error: " << e.what();
    //     }
    // }

}// namespace Euclid::Database