//
// Created by vogje01 on 8/20/26.
//

// C++ includes
#include <chrono>

// Euclid includes
#include <euclid/database/repository/esm/MongoEsmRepository.h>

namespace Euclid::Database {

    MongoEsmRepository::MongoEsmRepository() {
        ensureIndexes();
    }

    void MongoEsmRepository::ensureIndexes() {

        try {
            const auto entry = Database::instance().client();
            auto bucketCollection = (*entry)[Database::instance().databaseName()][BUCKET_COLLECTION];

            // Compound on (accountId, namespace, name) rather than name alone - bucket names only
            // need to be unique within their own account/namespace, not globally. NOTE: replacing
            // a pre-existing unique index on "name" alone requires dropping that old index first
            // (Mongo won't do this automatically), and will fail if duplicate names already exist
            // across accounts in production data - this is an operational migration step.
            mongocxx::options::index nameOpts;
            nameOpts.unique(true);
            bucketCollection.create_index(make_document(kvp("accountId", 1), kvp("namespace", 1), kvp("name", 1)), nameOpts);

            mongocxx::options::index ernOpts;
            ernOpts.unique(true);
            bucketCollection.create_index(make_document(kvp("ern", 1)), ernOpts);

            auto objectCollection = (*entry)[Database::instance().databaseName()][OBJECT_COLLECTION];

            mongocxx::options::index objectKeyOpts;
            objectKeyOpts.unique(true);
            objectCollection.create_index(make_document(kvp("bucketErn", 1), kvp("key", 1)), objectKeyOpts);

            mongocxx::options::index objectErnOpts;
            objectErnOpts.unique(true);
            objectCollection.create_index(make_document(kvp("ern", 1)), objectErnOpts);

        } catch (const std::exception &e) {
            log_error << "Ensure storage indexes failed, error: " << e.what();
        }
    }

    bool MongoEsmRepository::bucketExists(const std::string &name) const {

        try {

            document query{};
            if (!name.empty()) {
                query.append(kvp("name", name));
            }

            const auto entry = Database::instance().client();
            auto bucketCollection = (*entry)[Database::instance().databaseName()][BUCKET_COLLECTION];

            const auto result = bucketCollection.find_one(query.extract());
            log_trace << "Bucket exists, name: " << name << ", exists: " << std::boolalpha << result.has_value();
            return result.has_value();

        } catch (const std::exception &e) {
            log_error << "Bucket exists failed, name: " << name << ", error: " << e.what();
        }
        return false;
    }

    std::optional<Entity::ESM::Bucket> MongoEsmRepository::findBucketById(const std::string &oid) const {

        try {

            document document;
            document.append(kvp("_id", oid));

            const auto entry = Database::instance().client();
            auto bucketCollection = (*entry)[Database::instance().databaseName()][BUCKET_COLLECTION];

            if (auto mResult = bucketCollection.find_one(document.view())) {
                return Entity::ESM::Bucket::fromDocument(mResult->view());
            }

        } catch (const std::exception &e) {
            log_error << "Get bucket by ID failed, error: " << e.what();
        }
        return {};
    }

    std::optional<Entity::ESM::Bucket> MongoEsmRepository::findBucketByName(const std::string &name) const {

        try {

            const auto entry = Database::instance().client();
            auto bucketCollection = (*entry)[Database::instance().databaseName()][BUCKET_COLLECTION];

            if (auto mResult = bucketCollection.find_one(make_document(kvp("name", name)))) {
                return Entity::ESM::Bucket::fromDocument(mResult.value());
            }

        } catch (const std::exception &e) {
            log_error << "Get bucket by name failed, name: " << name << ", error: " << e.what();
        }
        return {};
    }

    std::optional<Entity::ESM::Bucket> MongoEsmRepository::findBucketByErn(const std::string &ern) const {

        try {

            const auto entry = Database::instance().client();
            auto bucketCollection = (*entry)[Database::instance().databaseName()][BUCKET_COLLECTION];

            if (auto mResult = bucketCollection.find_one(make_document(kvp("ern", ern)))) {
                return Entity::ESM::Bucket::fromDocument(mResult.value());
            }

        } catch (const std::exception &e) {
            log_error << "Get bucket by ERN failed, ern: " << ern << ", error: " << e.what();
        }
        return {};
    }

    std::vector<Entity::ESM::Bucket> MongoEsmRepository::listBuckets(const std::string &accountId, const std::string &namespaceName, const std::string &prefix, const long pageSize, const long pageIndex, const std::string &sortColumn) const {

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
                opts.sort(make_document(kvp(sortColumn, 1)));
            }
            if (pageSize > 0) {
                opts.limit(pageSize);
                opts.skip(std::max<long>(pageIndex, 0) * pageSize);
            }

            std::vector<Entity::ESM::Bucket> buckets;
            const auto entry = Database::instance().client();
            auto bucketCollection = (*entry)[Database::instance().databaseName()][BUCKET_COLLECTION];

            for (auto bucketCursor = bucketCollection.find(filter.view(), opts); auto bucket: bucketCursor) {
                buckets.push_back(Entity::ESM::Bucket::fromDocument(bucket));
            }
            return buckets;

        } catch (const std::exception &e) {

            log_error << "List buckets failed, error: " << e.what();
            return {};
        }
    }

    Entity::ESM::Bucket MongoEsmRepository::upsertBucket(Entity::ESM::Bucket &bucket) {

        try {

            const auto filter = make_document(kvp("accountId", bucket.accountId), kvp("namespace", bucket.nameSpace), kvp("name", bucket.name));
            const auto update = make_document(
                    kvp("$set", bucket.toDocument()),
                    kvp("$setOnInsert", make_document(
                                kvp("created", bsoncxx::types::b_date{
                                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                                    bucket.created.time_since_epoch())})
                                )),
                    kvp("$currentDate", make_document(
                                kvp("modified", true)
                                )));

            mongocxx::options::find_one_and_update opts;
            opts.upsert(true);
            opts.return_document(mongocxx::options::return_document::k_after);

            const auto entry = Database::instance().client();
            auto bucketCollection = (*entry)[Database::instance().databaseName()][BUCKET_COLLECTION];

            if (auto result = bucketCollection.find_one_and_update(filter.view(), update.view(), opts)) {
                return Entity::ESM::Bucket::fromDocument(result->view());
            }

        } catch (const std::exception &e) {
            log_error << "Upsert bucket failed, error: " << e.what();
        }
        return bucket;
    }

    long MongoEsmRepository::countBuckets(const std::string &accountId, const std::string &namespaceName) const {

        try {

            document filter = {};
            filter.append(kvp("accountId", accountId));
            if (!namespaceName.empty()) {
                filter.append(kvp("namespace", namespaceName));
            }

            const auto entry = Database::instance().client();
            auto bucketCollection = (*entry)[Database::instance().databaseName()][BUCKET_COLLECTION];

            const int64_t count = bucketCollection.count_documents(filter.extract());
            return static_cast<long>(count);

        } catch (const std::exception &e) {

            log_error << "Count buckets failed, error: " << e.what();
        }
        return -1;
    }

    void MongoEsmRepository::removeBucketByName(const std::string &name) {

        try {
            const auto entry = Database::instance().client();
            auto bucketCollection = (*entry)[Database::instance().databaseName()][BUCKET_COLLECTION];

            const auto result = bucketCollection.delete_many(make_document(kvp("name", name)));
            log_debug << "Bucket deleted, count: " << result->deleted_count();

        } catch (const std::exception &e) {
            log_error << "Delete bucket failed, error: " << e.what();
        }
    }

    void MongoEsmRepository::deleteBucketByErn(const std::string &ern) {

        try {
            const auto entry = Database::instance().client();
            auto bucketCollection = (*entry)[Database::instance().databaseName()][BUCKET_COLLECTION];

            const auto result = bucketCollection.delete_many(make_document(kvp("ern", ern)));
            log_debug << "Bucket deleted, count: " << result->deleted_count();

        } catch (const std::exception &e) {
            log_error << "Delete bucket failed, error: " << e.what();
        }
    }

    void MongoEsmRepository::clearBuckets() {

        try {
            const auto entry = Database::instance().client();
            auto bucketCollection = (*entry)[Database::instance().databaseName()][BUCKET_COLLECTION];

            const auto result = bucketCollection.delete_many({});
            log_debug << "All buckets deleted, count: " << result->deleted_count();

        } catch (const std::exception &e) {
            log_error << "Delete all buckets failed, error: " << e.what();
        }
    }

    Entity::ESM::Object MongoEsmRepository::upsertObject(Entity::ESM::Object &object) {

        try {

            const auto filter = make_document(kvp("bucketErn", object.bucketErn), kvp("key", object.key));
            const auto update = make_document(
                    kvp("$set", object.toDocument()),
                    kvp("$setOnInsert", make_document(
                                kvp("created", bsoncxx::types::b_date{
                                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                                    object.created.time_since_epoch())})
                                )),
                    kvp("$currentDate", make_document(
                                kvp("modified", true)
                                )));

            mongocxx::options::find_one_and_update opts;
            opts.upsert(true);
            opts.return_document(mongocxx::options::return_document::k_after);

            const auto entry = Database::instance().client();
            auto objectCollection = (*entry)[Database::instance().databaseName()][OBJECT_COLLECTION];

            if (auto result = objectCollection.find_one_and_update(filter.view(), update.view(), opts)) {
                return Entity::ESM::Object::fromDocument(result->view());
            }

        } catch (const std::exception &e) {
            log_error << "Upsert object failed, error: " << e.what();
        }
        return object;
    }

    std::optional<Entity::ESM::Object> MongoEsmRepository::findObjectByBucketAndKey(const std::string &bucketErn, const std::string &key) const {

        try {

            const auto entry = Database::instance().client();
            auto objectCollection = (*entry)[Database::instance().databaseName()][OBJECT_COLLECTION];

            if (auto mResult = objectCollection.find_one(make_document(kvp("bucketErn", bucketErn), kvp("key", key)))) {
                return Entity::ESM::Object::fromDocument(mResult.value());
            }

        } catch (const std::exception &e) {
            log_error << "Get object by bucket/key failed, bucketErn: " << bucketErn << ", key: " << key << ", error: " << e.what();
        }
        return {};
    }

    std::optional<Entity::ESM::Object> MongoEsmRepository::findObjectByErn(const std::string &ern) const {

        try {

            const auto entry = Database::instance().client();
            auto objectCollection = (*entry)[Database::instance().databaseName()][OBJECT_COLLECTION];

            if (auto mResult = objectCollection.find_one(make_document(kvp("ern", ern)))) {
                return Entity::ESM::Object::fromDocument(mResult.value());
            }

        } catch (const std::exception &e) {
            log_error << "Get object by ern failed, ern: " << ern << ", error: " << e.what();
        }
        return {};
    }

    long MongoEsmRepository::countObjects() const {

        try {
            const auto entry = Database::instance().client();
            auto messageCollection = (*entry)[Database::instance().databaseName()][OBJECT_COLLECTION];

            return messageCollection.count_documents({});
        } catch (const std::exception &e) {
            log_error << "Count messages failed, error: " << e.what();
        }
        return -1;
    }

    long MongoEsmRepository::countObjects(const std::string &bucketErn, const std::string &prefix) const {

        try {
            document filter = {};
            filter.append(kvp("bucketErn", bucketErn));
            if (!prefix.empty()) {
                filter.append(kvp("key", make_document(kvp("$regex", "^" + prefix))));
            }
            const auto entry = Database::instance().client();
            auto messageCollection = (*entry)[Database::instance().databaseName()][OBJECT_COLLECTION];

            return messageCollection.count_documents(filter.view());
        } catch (const std::exception &e) {
            log_error << "Count objects failed, ern: " << bucketErn << ", error: " << e.what();
        }
        return -1;
    }

    std::vector<Entity::ESM::Object> MongoEsmRepository::listObjects(const std::string &bucketErn, const std::string &prefix, const long pageSize, const long pageIndex, const std::string &sortColumn) const {

        try {

            document filter = {};
            filter.append(kvp("bucketErn", bucketErn));
            if (!prefix.empty()) {
                filter.append(kvp("key", make_document(kvp("$regex", "^" + prefix))));
            }

            mongocxx::options::find opts;
            if (!sortColumn.empty()) {
                opts.sort(make_document(kvp(sortColumn, 1)));
            }
            if (pageSize > 0) {
                opts.limit(pageSize);
                opts.skip(std::max<long>(pageIndex, 0) * pageSize);
            }

            std::vector<Entity::ESM::Object> objects;
            const auto entry = Database::instance().client();
            auto objectCollection = (*entry)[Database::instance().databaseName()][OBJECT_COLLECTION];

            for (auto objectCursor = objectCollection.find(filter.view(), opts); auto object: objectCursor) {
                objects.push_back(Entity::ESM::Object::fromDocument(object));
            }
            return objects;

        } catch (const std::exception &e) {

            log_error << "List objects failed, error: " << e.what();
            return {};
        }
    }

    void MongoEsmRepository::deleteObjectByErn(const std::string &ern) {

        try {
            const auto entry = Database::instance().client();
            auto bucketCollection = (*entry)[Database::instance().databaseName()][OBJECT_COLLECTION];

            const auto result = bucketCollection.delete_many(make_document(kvp("ern", ern)));
            log_debug << "Object deleted, count: " << result->deleted_count();

        } catch (const std::exception &e) {
            log_error << "Delete object failed, error: " << e.what();
        }
    }

}// namespace Euclid::Database