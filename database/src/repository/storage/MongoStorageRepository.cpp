//
// Created by vogje01 on 8/20/26.
//

// C++ includes
#include <chrono>

// Euclid includes
#include <euclid/database/repository/storage/MongoStorageRepository.h>

namespace Euclid::Database {

    MongoStorageRepository::MongoStorageRepository() : _databaseName("euclid") {
        ensureIndexes();
    }

    void MongoStorageRepository::ensureIndexes() const {

        try {
            const auto entry = Database::instance().client();
            auto bucketCollection = (*entry)[Database::instance().databaseName()][BUCKET_COLLECTION];

            mongocxx::options::index nameOpts;
            nameOpts.unique(true);
            bucketCollection.create_index(make_document(kvp("name", 1)), nameOpts);

            mongocxx::options::index ernOpts;
            ernOpts.unique(true);
            bucketCollection.create_index(make_document(kvp("ern", 1)), ernOpts);

        } catch (const std::exception &e) {
            log_error << "Ensure storage indexes failed, error: " << e.what();
        }
    }

    bool MongoStorageRepository::bucketExists(const std::string &name) const {

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

    std::optional<Entity::Storage::Bucket> MongoStorageRepository::findBucketById(const std::string &oid) const {

        try {

            document document;
            document.append(kvp("_id", oid));

            const auto entry = Database::instance().client();
            auto bucketCollection = (*entry)[Database::instance().databaseName()][BUCKET_COLLECTION];

            if (auto mResult = bucketCollection.find_one(document.view())) {
                return Entity::Storage::Bucket::fromDocument(mResult->view());
            }

        } catch (const std::exception &e) {
            log_error << "Get bucket by ID failed, error: " << e.what();
        }
        return {};
    }

    std::optional<Entity::Storage::Bucket> MongoStorageRepository::findBucketByName(const std::string &name) const {

        try {

            const auto entry = Database::instance().client();
            auto bucketCollection = (*entry)[Database::instance().databaseName()][BUCKET_COLLECTION];

            if (auto mResult = bucketCollection.find_one(make_document(kvp("name", name)))) {
                return Entity::Storage::Bucket::fromDocument(mResult.value());
            }

        } catch (const std::exception &e) {
            log_error << "Get bucket by name failed, name: " << name << ", error: " << e.what();
        }
        return {};
    }

    std::optional<Entity::Storage::Bucket> MongoStorageRepository::findBucketByErn(const std::string &ern) const {

        try {

            const auto entry = Database::instance().client();
            auto bucketCollection = (*entry)[Database::instance().databaseName()][BUCKET_COLLECTION];

            if (auto mResult = bucketCollection.find_one(make_document(kvp("ern", ern)))) {
                return Entity::Storage::Bucket::fromDocument(mResult.value());
            }

        } catch (const std::exception &e) {
            log_error << "Get bucket by ERN failed, ern: " << ern << ", error: " << e.what();
        }
        return {};
    }

    std::vector<Entity::Storage::Bucket> MongoStorageRepository::listBuckets(const std::string &prefix, const long pageSize, const long pageIndex, const std::string &sortColumn) const {

        try {

            document filter = {};
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

            std::vector<Entity::Storage::Bucket> buckets;
            const auto entry = Database::instance().client();
            auto bucketCollection = (*entry)[Database::instance().databaseName()][BUCKET_COLLECTION];

            for (auto bucketCursor = bucketCollection.find(filter.view(), opts); auto bucket: bucketCursor) {
                buckets.push_back(Entity::Storage::Bucket::fromDocument(bucket));
            }
            return buckets;

        } catch (const std::exception &e) {

            log_error << "List buckets failed, error: " << e.what();
            return {};
        }
    }

    Entity::Storage::Bucket MongoStorageRepository::upsertBucket(Entity::Storage::Bucket &bucket) {

        try {

            const auto filter = make_document(kvp("name", bucket.name));
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
                return Entity::Storage::Bucket::fromDocument(result->view());
            }

        } catch (const std::exception &e) {
            log_error << "Upsert bucket failed, error: " << e.what();
        }
        return bucket;
    }

    long MongoStorageRepository::countBuckets() const {

        try {

            const auto entry = Database::instance().client();
            auto bucketCollection = (*entry)[Database::instance().databaseName()][BUCKET_COLLECTION];

            const int64_t count = bucketCollection.count_documents({});
            return static_cast<long>(count);

        } catch (const std::exception &e) {

            log_error << "Count buckets failed, error: " << e.what();
        }
        return -1;
    }

    void MongoStorageRepository::removeBucketByName(const std::string &name) {

        try {
            const auto entry = Database::instance().client();
            auto bucketCollection = (*entry)[Database::instance().databaseName()][BUCKET_COLLECTION];

            const auto result = bucketCollection.delete_many(make_document(kvp("name", name)));
            log_debug << "Bucket deleted, count: " << result->deleted_count();

        } catch (const std::exception &e) {
            log_error << "Delete bucket failed, error: " << e.what();
        }
    }

    void MongoStorageRepository::deleteBucketByErn(const std::string &ern) {

        try {
            const auto entry = Database::instance().client();
            auto bucketCollection = (*entry)[Database::instance().databaseName()][BUCKET_COLLECTION];

            const auto result = bucketCollection.delete_many(make_document(kvp("ern", ern)));
            log_debug << "Bucket deleted, count: " << result->deleted_count();

        } catch (const std::exception &e) {
            log_error << "Delete bucket failed, error: " << e.what();
        }
    }

    void MongoStorageRepository::clearBuckets() {

        try {
            const auto entry = Database::instance().client();
            auto bucketCollection = (*entry)[Database::instance().databaseName()][BUCKET_COLLECTION];

            const auto result = bucketCollection.delete_many({});
            log_debug << "All buckets deleted, count: " << result->deleted_count();

        } catch (const std::exception &e) {
            log_error << "Delete all buckets failed, error: " << e.what();
        }
    }

}// namespace Euclid::Database
