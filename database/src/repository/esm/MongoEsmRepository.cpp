//
// Created by vogje01 on 8/20/26.
//

// C++ includes
#include <chrono>

// Euclid includes
#include <mongocxx/pipeline.hpp>

#include <euclid/database/repository/esm/MongoEsmRepository.h>

#include "euclid/core/monitoring/MonitoringTimer.h"

namespace Euclid::Database {

    namespace {

        // A prefix is a literal, not a pattern. Object keys are full of regex metacharacters -
        // every file extension contributes a "." - so an unescaped prefix quietly matches more
        // than was asked for: "jvo/file.xml" would also select "jvo/fileXxml". Harmless in a
        // listing, not harmless in touch-object or purge-bucket, which act on what it selects.
        std::string QuotePrefix(const std::string &prefix) {
            std::string quoted;
            quoted.reserve(prefix.size());
            for (const char c: prefix) {
                if (std::string_view(R"(\^$.|?*+()[]{})").contains(c)) quoted += '\\';
                quoted += c;
            }
            return quoted;
        }

    }// namespace


    namespace {
        // Where this repository's time actually goes, one series per operation - the same pair of
        // metrics the modules record for their actions ("esm-service-time"/"esm-service-count",
        // labelled by method), one layer down and labelled by repository operation instead.
        //
        // The point of measuring here rather than at the action is that an action's cost is not
        // its database cost: "receive-messages" spends most of a long poll asleep, and what it
        // does against the database in between is several queries per 100ms attempt. Only the
        // operations below are timed, never the waiting.
        constexpr auto kRepositoryTimer = "esm-repository-time";
        constexpr auto kRepositoryCounter = "esm-repository-count";
    }

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

            // Serves recountBuckets(): every field its pipeline names is here and in this order,
            // so the whole pass is an index scan with no document fetched at all. Without it the
            // recount reads the entire object collection several times a minute - measured at 2.3
            // seconds over 2.4 million objects, on the same database everything else is using.
            //
            // "status" leads because the pipeline matches on it; "bucketErn" follows because that
            // is what the result is grouped by, so the index also delivers the groups in order.
            objectCollection.create_index(make_document(kvp("status", 1), kvp("bucketErn", 1), kvp("directory", 1), kvp("size", 1)));

            auto subscriptionCollection = (*entry)[Database::instance().databaseName()][SUBSCRIPTION_COLLECTION];
            mongocxx::options::index subscriptionOpts;
            subscriptionOpts.unique(true);
            subscriptionCollection.create_index(make_document(kvp("sourceErn", 1), kvp("type", 1), kvp("targetErn", 1)), subscriptionOpts);

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

    std::vector<Entity::ESM::Bucket> MongoEsmRepository::listBuckets(const std::string &accountId, const std::string &namespaceName, const std::string &prefix, const long pageSize, const long pageIndex, const std::string &sortColumn, const std::string &sortDirection, const bool includeInternal) const {

        try {

            document filter = {};
            filter.append(kvp("accountId", accountId));
            if (!namespaceName.empty()) {
                filter.append(kvp("namespace", namespaceName));
            }
            if (!prefix.empty()) {
                filter.append(kvp("name", make_document(kvp("$regex", "^" + QuotePrefix(prefix)))));
            }

            // euclid's own buckets are not somebody's to look at, unless the caller asked for
            // them and is entitled to (the module decides that, not this layer). "$ne" rather than
            // "false": a bucket made before this field existed has no "internal" at all, and
            // equality would leave every one of them out of its owner's listing.
            if (!includeInternal) {
                filter.append(kvp("internal", make_document(kvp("$ne", true))));
            }

            mongocxx::options::find opts;
            if (!sortColumn.empty()) {
                opts.sort(make_document(kvp(sortColumn, sortDirection == "asc" ? 1 : -1)));
            }
            if (pageSize > 0) {
                opts.limit(pageSize);
                opts.skip(std::max<long>(pageIndex, 0) * pageSize);
            }

            std::vector<Entity::ESM::Bucket> buckets;
            const auto entry = Database::instance().client();
            auto bucketCollection = (*entry)[Database::instance().databaseName()][BUCKET_COLLECTION];

            for (auto bucketCursor = bucketCollection.find(filter.view(), opts); auto bucket: bucketCursor) {
                // Per document, not around the loop: Bucket::fromDocument() reads every field with
                // a typed accessor, and one field of an unexpected type throws. Caught out here
                // that took the whole listing with it - a single malformed row and list-buckets
                // answered with no buckets at all while the count still reported them, which is
                // exactly what one hand-edited "internal": "true" (a string) did. A row that
                // cannot be read is dropped and named in the log instead.
                try {
                    buckets.push_back(Entity::ESM::Bucket::fromDocument(bucket));
                } catch (const std::exception &e) {
                    log_error << "Skipping unreadable bucket document, error: " << e.what();
                }
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
            throw std::runtime_error("upsert returned no document, name: " + bucket.name);

        } catch (const std::exception &e) {
            log_error << "Upsert bucket failed, error: " << e.what();
            throw;
        }
    }

    long MongoEsmRepository::countBuckets(const std::string &accountId, const std::string &namespaceName, const std::string &prefix, const bool includeInternal) const {

        try {

            document filter = {};
            filter.append(kvp("accountId", accountId));
            if (!namespaceName.empty()) {
                filter.append(kvp("namespace", namespaceName));
            }
            if (!prefix.empty()) {
                filter.append(kvp("name", make_document(kvp("$regex", "^" + QuotePrefix(prefix)))));
            }

            // euclid's own buckets are not somebody's to look at, unless the caller asked for
            // them and is entitled to (the module decides that, not this layer). "$ne" rather than
            // "false": a bucket made before this field existed has no "internal" at all, and
            // equality would leave every one of them out of its owner's listing.
            if (!includeInternal) {
                filter.append(kvp("internal", make_document(kvp("$ne", true))));
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

        // Derived here rather than by each caller, so it cannot disagree with the key it describes
        // - the counters read this field and never look at the key.
        object.directory = Entity::ESM::IsDirectoryKey(object.key);

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
            throw std::runtime_error("upsert returned no document, key: " + object.key);

        } catch (const std::exception &e) {
            log_error << "Upsert object failed, error: " << e.what();
            throw;
        }
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

    long MongoEsmRepository::countObjects(const std::string &bucketErn, const std::string &prefix, const bool includeDirectories) const {

        try {
            document filter = {};
            filter.append(kvp("bucketErn", bucketErn));
            if (!prefix.empty()) {
                filter.append(kvp("key", make_document(kvp("$regex", "^" + QuotePrefix(prefix)))));
            }
            if (!includeDirectories) {
                filter.append(kvp("$nor", make_array(make_document(kvp("key", make_document(kvp("$regex", "/$")))))));
            }
            const auto entry = Database::instance().client();
            auto messageCollection = (*entry)[Database::instance().databaseName()][OBJECT_COLLECTION];

            return messageCollection.count_documents(filter.view());
        } catch (const std::exception &e) {
            log_error << "Count objects failed, ern: " << bucketErn << ", error: " << e.what();
        }
        return -1;
    }

    long MongoEsmRepository::countEncryptedObjects(const std::string &bucketErn) const {

        try {
            // Objects written before encryption at rest existed carry no "encryptionKeyErn" field
            // at all, so "not encrypted" has to mean missing or empty rather than empty alone -
            // $nin covers both without a second pass.
            const auto filter = make_document(
                    kvp("bucketErn", bucketErn),
                    kvp("encryptionKeyErn", make_document(kvp("$nin", make_array("", bsoncxx::types::b_null{})))));

            const auto entry = Database::instance().client();
            auto objectCollection = (*entry)[Database::instance().databaseName()][OBJECT_COLLECTION];

            return objectCollection.count_documents(filter.view());
        } catch (const std::exception &e) {
            log_error << "Count encrypted objects failed, ern: " << bucketErn << ", error: " << e.what();
        }
        return -1;
    }

    std::vector<Entity::ESM::Object> MongoEsmRepository::listObjects(const std::string &bucketErn, const std::string &prefix, const long pageSize, const long pageIndex, const std::string &sortColumn, const std::string &sortDirection, const bool includeDirectories) const {

        try {

            document filter = {};
            filter.append(kvp("bucketErn", bucketErn));
            if (!prefix.empty()) {
                filter.append(kvp("key", make_document(kvp("$regex", "^" + QuotePrefix(prefix)))));
            }
            // $nor rather than a second "key" condition: the prefix filter above already occupies
            // that field name, and a document cannot carry it twice.
            if (!includeDirectories) {
                filter.append(kvp("$nor", make_array(make_document(kvp("key", make_document(kvp("$regex", "/$")))))));
            }

            mongocxx::options::find opts;
            if (!sortColumn.empty()) {
                opts.sort(make_document(kvp(sortColumn, sortDirection == "asc" ? 1 : -1)));
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

    std::optional<Entity::ESM::Bucket> MongoEsmRepository::renameBucket(const std::string &ern, const std::string &newName,
                                                                        const std::string &newErn) {

        try {
            const auto entry = Database::instance().client();
            auto bucketCollection = (*entry)[Database::instance().databaseName()][BUCKET_COLLECTION];

            const auto update = make_document(
                    kvp("$set", make_document(kvp("name", newName), kvp("ern", newErn))),
                    kvp("$currentDate", make_document(kvp("modified", true))));

            mongocxx::options::find_one_and_update opts;
            opts.return_document(mongocxx::options::return_document::k_after);

            if (auto result = bucketCollection.find_one_and_update(make_document(kvp("ern", ern)).view(), update.view(), opts)) {
                return Entity::ESM::Bucket::fromDocument(result->view());
            }
            return std::nullopt;

        } catch (const std::exception &e) {
            log_error << "Rename bucket failed, error: " << e.what();
            return std::nullopt;
        }
    }

    long MongoEsmRepository::repointSubscriptions(const std::string &oldSourceErn, const std::string &newSourceErn) {

        try {
            const auto entry = Database::instance().client();
            auto subscriptionCollection = (*entry)[Database::instance().databaseName()][SUBSCRIPTION_COLLECTION];

            const auto result = subscriptionCollection.update_many(
                    make_document(kvp("sourceErn", oldSourceErn)).view(),
                    make_document(kvp("$set", make_document(kvp("sourceErn", newSourceErn)))).view());
            return result ? static_cast<long>(result->modified_count()) : 0;

        } catch (const std::exception &e) {
            log_error << "Repoint subscriptions failed, error: " << e.what();
            return 0;
        }
    }

    long MongoEsmRepository::renameBucketObjects(const std::string &oldBucketErn, const std::string &newBucketErn,
                                                 const std::string &oldName, const std::string &newName) {

        try {
            const auto entry = Database::instance().client();
            auto objectCollection = (*entry)[Database::instance().databaseName()][OBJECT_COLLECTION];

            // An update pipeline rather than a read-modify-write per object: the new bucketErn is
            // the same for every one of them, and the ERN differs only in the bucket segment,
            // which the server can rewrite in place. A bucket with a hundred thousand objects is
            // then one round trip instead of a hundred thousand.
            //
            // Only the bucket segment is touched - ":object:<bucket>/" - so a key that happens to
            // contain the bucket's old name somewhere inside it is left alone.
            const auto oldSegment = ":object:" + oldName + "/";
            const auto newSegment = ":object:" + newName + "/";

            mongocxx::pipeline pipeline;
            pipeline.add_fields(make_document(
                    kvp("bucketErn", newBucketErn),
                    kvp("ern", make_document(kvp("$replaceOne", make_document(
                                                         kvp("input", "$ern"),
                                                         kvp("find", oldSegment),
                                                         kvp("replacement", newSegment)))))));

            const auto result = objectCollection.update_many(make_document(kvp("bucketErn", oldBucketErn)), pipeline);
            const auto renamed = result ? static_cast<long>(result->modified_count()) : 0;
            log_debug << "Bucket objects renamed, count: " << renamed << ", bucket: " << newName;
            return renamed;

        } catch (const std::exception &e) {
            log_error << "Rename bucket objects failed, error: " << e.what();
            return 0;
        }
    }

    Entity::ESM::Subscription MongoEsmRepository::upsertSubscription(Entity::ESM::Subscription &subscription) {

        try {

            const auto filter = make_document(kvp("sourceErn", subscription.sourceErn), kvp("type", subscription.type), kvp("targetErn", subscription.targetErn));
            const auto update = make_document(
                    kvp("$set", subscription.toDocument()),
                    kvp("$setOnInsert", make_document(
                                kvp("created", bsoncxx::types::b_date{
                                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                                    subscription.created.time_since_epoch())
                                    })
                                )),
                    kvp("$currentDate", make_document(
                                kvp("modified", true)
                                )));

            mongocxx::options::find_one_and_update opts;
            opts.upsert(true);
            opts.return_document(mongocxx::options::return_document::k_after);

            const auto entry = Database::instance().client();
            auto subscriptionCollection = (*entry)[Database::instance().databaseName()][SUBSCRIPTION_COLLECTION];

            if (auto result = subscriptionCollection.find_one_and_update(filter.view(), update.view(), opts)) {
                return Entity::ESM::Subscription::fromDocument(result->view());
            }
            throw std::runtime_error("upsert returned no document, sourceErn: " + subscription.sourceErn);

        } catch (const std::exception &e) {
            log_error << "Upsert subscription failed, error: " << e.what();
            throw;
        }
    }

    std::vector<Entity::ESM::Subscription> MongoEsmRepository::listSubscriptionsBySourceErn(const std::string &sourceErn) const {

        std::vector<Entity::ESM::Subscription> subscriptions;
        try {
            const auto filter = make_document(kvp("sourceErn", sourceErn));

            const auto entry = Database::instance().client();
            auto subscriptionCollection = (*entry)[Database::instance().databaseName()][SUBSCRIPTION_COLLECTION];

            for (auto cursor = subscriptionCollection.find(filter.view()); auto doc: cursor) {
                subscriptions.push_back(Entity::ESM::Subscription::fromDocument(doc));
            }

        } catch (const std::exception &e) {
            log_error << "List subscriptions failed, sourceErn: " << sourceErn << ", error: " << e.what();
        }
        return subscriptions;
    }

    void MongoEsmRepository::deleteSubscriptionByErn(const std::string &ern) {

        try {
            const auto entry = Database::instance().client();
            auto subscriptionCollection = (*entry)[Database::instance().databaseName()][SUBSCRIPTION_COLLECTION];

            const auto result = subscriptionCollection.delete_many(make_document(kvp("ern", ern)));
            log_debug << "Subscription deleted, ern: " << ern << ", count: " << result->deleted_count();

        } catch (const std::exception &e) {
            log_error << "Delete subscription failed, ern: " << ern << ", error: " << e.what();
        }
    }

    void MongoEsmRepository::recountBuckets() {

        Core::Monitoring::MonitoringTimer measure(kRepositoryTimer, kRepositoryCounter, "operation", "recount-buckets");

        try {
            const auto entry = Database::instance().client();
            auto objectCollection = (*entry)[Database::instance().databaseName()][OBJECT_COLLECTION];
            auto bucketCollection = (*entry)[Database::instance().databaseName()][BUCKET_COLLECTION];

            // One pass over the objects, with the accumulators the loop below actually reads.
            //
            // What is counted has to match what put-object and delete-object maintain incrementally,
            // or the recount would "correct" the counters to a different definition every time it
            // ran: every object contributes its bytes, but a directory marker (a zero-byte key
            // ending in "/") is not counted as an object, exactly as EsmServer does when it stores
            // one. Only COMPLETED objects count - a multipart upload in progress has rows that no
            // bucket counter has been told about yet.
            mongocxx::pipeline pipeline;
            pipeline.match(make_document(kvp("status", "COMPLETED")));
            pipeline.group(make_document(
                    kvp("_id", "$bucketErn"),
                    kvp("bytes", make_document(kvp("$sum", "$size"))),
                    // The stored flag, not a test on the key. Reading the key here meant a regular
                    // expression evaluated against every object in the installation on every pass,
                    // and - far more expensive - it meant every document had to be fetched to
                    // answer it. Naming only fields the index carries lets this be served from the
                    // index alone. An object written before the flag existed has no value and
                    // counts as a file; see Entity::ESM::Object::directory.
                    kvp("count", make_document(kvp("$sum", make_document(kvp("$cond", make_array(
                                                                                 make_document(kvp("$ifNull", make_array("$directory", false))),
                                                                                 0, 1))))))));

            struct Counts {
                long count{};
                long size{};
            };
            std::unordered_map<std::string, Counts> counted;

            // $sum returns whichever numeric type the values fit in, so the width is not ours to
            // assume: a bucket of small objects comes back int32 and the same bucket comes back
            // int64 once it grows, and get_int64() on the first would throw.
            auto asLong = [](const bsoncxx::document::element &field) -> long {
                if (!field) return 0;
                switch (field.type()) {
                    case bsoncxx::type::k_int64:
                        return static_cast<long>(field.get_int64().value);
                    case bsoncxx::type::k_int32:
                        return field.get_int32().value;
                    case bsoncxx::type::k_double:
                        return static_cast<long>(field.get_double().value);
                    default:
                        return 0;
                }
            };

            for (auto cursor = objectCollection.aggregate(pipeline); auto doc: cursor) {
                const auto idField = doc["_id"];
                if (!idField || idField.type() != bsoncxx::type::k_string) continue;

                auto &counts = counted[std::string(idField.get_string().value)];
                counts.count = asLong(doc["count"]);
                counts.size = asLong(doc["bytes"]);
            }

            // Every bucket is written, including the ones the grouping did not mention: a bucket
            // that has just been emptied is absent from it, and leaving its last non-zero counters
            // standing is exactly the drift this replaces.
            long buckets = 0;
            for (auto cursor = bucketCollection.find({}); auto doc: cursor) {
                const auto ernField = doc["ern"];
                if (!ernField || ernField.type() != bsoncxx::type::k_string) continue;
                const auto ern = std::string(ernField.get_string().value);

                const auto it = counted.find(ern);
                const Counts counts = it != counted.end() ? it->second : Counts{};
                bucketCollection.update_one(make_document(kvp("ern", ern)).view(),
                                            make_document(kvp("$set", make_document(
                                                                      kvp("objects", static_cast<int64_t>(counts.count)),
                                                                      kvp("size", static_cast<int64_t>(counts.size)))))
                                            .view());
                ++buckets;
            }

            log_debug << "Bucket counts recounted, buckets: " << buckets;

        } catch (const std::exception &e) {
            log_error << "Bucket recount failed, error: " << e.what();
        }
    }

}// namespace Euclid::Database