// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/20/26.
//

// C++ includes
#include <algorithm>
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Euclid includes
#include <mongocxx/pipeline.hpp>

#include <euclid/core/Configuration.h>
#include <euclid/core/TtlCache.h>
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

        /**
         * @brief Each bucket's subscriptions, by bucket ERN - see listSubscriptionsBySourceErnCached().
         *
         * @par
         * Every object written or removed asks which queues to tell, and on a split that is a
         * million objects into one bucket whose subscriptions do not change while it runs. Short by
         * default because ESM runs as a pool: a subscription made through another instance is only
         * seen here when the entry runs out, and an event in between is not delivered to it. The TTL
         * is read once, when the first lookup builds this.
         */
        Core::TtlCache<std::string, std::vector<Entity::ESM::Subscription> > &subscriptionCache() {
            // Misses off: a miss here is a read that failed, not a bucket with no subscriptions -
            // that is an empty list, and is cached like any other answer.
            static Core::TtlCache<std::string, std::vector<Entity::ESM::Subscription> > cache{
                    std::chrono::seconds(std::max(0L, Core::Configuration::instance().getOr<long>("euclid.modules.esm.subscription-cache-seconds", 5))),
                    4096, false};
            return cache;
        }
    }

    std::vector<Entity::ESM::Subscription> MongoEsmRepository::listSubscriptionsBySourceErnCached(const std::string &sourceErn) const {
        return subscriptionCache()
                .get(sourceErn, [](const std::string &key) { return loadSubscriptions(key); })
                .value_or(std::vector<Entity::ESM::Subscription>{});
    }

    MongoEsmRepository::MongoEsmRepository() {
        // Deferred rather than done here: on the EMD backend a repository can be constructed before
        // the module that holds the store is listening, and a constructor gets no second chance.
        Database::instance().onReachable(&ensureIndexes);
    }

    void MongoEsmRepository::ensureIndexes() {

        try {
            auto bucketCollection = Database::instance().collection(BUCKET_COLLECTION);

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

            auto objectCollection = Database::instance().collection(OBJECT_COLLECTION);

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

            ensureLevelIndex();

            auto subscriptionCollection = Database::instance().collection(SUBSCRIPTION_COLLECTION);
            mongocxx::options::index subscriptionOpts;
            subscriptionOpts.unique(true);
            subscriptionCollection.create_index(make_document(kvp("sourceErn", 1), kvp("type", 1), kvp("targetErn", 1)), subscriptionOpts);

        } catch (const std::exception &e) {
            log_error << "Ensure storage indexes failed, error: " << e.what();
        }
    }

    bool MongoEsmRepository::bucketExists(const std::string &accountId, const std::string &nameSpace, const std::string &name) const {

        try {

            // All three fields, in the order the unique index names them, so the answer is about
            // the caller's own bucket rather than about anyone in the installation holding the name.
            const auto query = make_document(kvp("accountId", accountId), kvp("namespace", nameSpace), kvp("name", name));

            auto bucketCollection = Database::instance().collection(BUCKET_COLLECTION);

            const auto result = bucketCollection.find_one(query.view());
            log_trace << "Bucket exists, accountId: " << accountId << ", namespace: " << nameSpace << ", name: " << name << ", exists: " << std::boolalpha << result.has_value();
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

            auto bucketCollection = Database::instance().collection(BUCKET_COLLECTION);

            if (auto mResult = bucketCollection.find_one(document.view())) {
                return Entity::ESM::Bucket::fromDocument(mResult->view());
            }

        } catch (const std::exception &e) {
            log_error << "Get bucket by ID failed, error: " << e.what();
        }
        return {};
    }

    std::optional<Entity::ESM::Bucket> MongoEsmRepository::findBucketByName(const std::string &accountId, const std::string &nameSpace, const std::string &name) const {

        try {

            auto bucketCollection = Database::instance().collection(BUCKET_COLLECTION);

            // A name identifies a bucket only together with the account and namespace that own it,
            // so resolving one without them would hand a caller somebody else's bucket - which is
            // also how it would have read its objects, since everything downstream works off the
            // ERN this returns.
            const auto filter = make_document(kvp("accountId", accountId), kvp("namespace", nameSpace), kvp("name", name));
            if (auto mResult = bucketCollection.find_one(filter.view())) {
                return Entity::ESM::Bucket::fromDocument(mResult.value());
            }

        } catch (const std::exception &e) {
            log_error << "Get bucket by name failed, name: " << name << ", error: " << e.what();
        }
        return {};
    }

    std::optional<Entity::ESM::Bucket> MongoEsmRepository::findBucketByErn(const std::string &ern) const {

        try {

            auto bucketCollection = Database::instance().collection(BUCKET_COLLECTION);

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
            auto bucketCollection = Database::instance().collection(BUCKET_COLLECTION);

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

    void MongoEsmRepository::adjustBucketCounters(const std::string &bucketErn, const long sizeDelta, const long objectDelta,
                                                 const long directoryDelta) {

        Core::Monitoring::MonitoringTimer measure(kRepositoryTimer, kRepositoryCounter, "operation", "adjustBucketCounters");

        if (sizeDelta == 0 && objectDelta == 0 && directoryDelta == 0) return;

        try {
            auto bucketCollection = Database::instance().collection(BUCKET_COLLECTION);
            const auto filter = make_document(kvp("ern", bucketErn));

            // One round trip, and the database does the addition - so two adjustments that overlap
            // both land instead of one overwriting the other.
            bucketCollection.update_one(filter.view(),
                                        make_document(kvp("$inc", make_document(
                                                                  kvp("size", static_cast<int64_t>(sizeDelta)),
                                                                  kvp("objects", static_cast<int64_t>(objectDelta)),
                                                                  kvp("directories", static_cast<int64_t>(directoryDelta)))))
                                                .view());

            // Only a subtraction can go below zero, so an upload pays for one round trip and not
            // three. See IEsmRepository::adjustBucketCounters for why this clamps at all.
            if (sizeDelta < 0) {
                bucketCollection.update_one(
                        make_document(kvp("ern", bucketErn), kvp("size", make_document(kvp("$lt", 0)))).view(),
                        make_document(kvp("$set", make_document(kvp("size", static_cast<int64_t>(0))))).view());
            }
            if (objectDelta < 0) {
                bucketCollection.update_one(
                        make_document(kvp("ern", bucketErn), kvp("objects", make_document(kvp("$lt", 0)))).view(),
                        make_document(kvp("$set", make_document(kvp("objects", static_cast<int64_t>(0))))).view());
            }
            if (directoryDelta < 0) {
                bucketCollection.update_one(
                        make_document(kvp("ern", bucketErn), kvp("directories", make_document(kvp("$lt", 0)))).view(),
                        make_document(kvp("$set", make_document(kvp("directories", static_cast<int64_t>(0))))).view());
            }

        } catch (const std::exception &e) {
            log_error << "Adjust bucket counters failed, ern: " << bucketErn << ", error: " << e.what();
            throw;
        }
    }

    // ── Background removals ──────────────────────────────────────────────────

    Entity::ESM::PurgeJob MongoEsmRepository::upsertPurgeJob(Entity::ESM::PurgeJob &job) {

        Core::Monitoring::MonitoringTimer measure(kRepositoryTimer, kRepositoryCounter, "operation", "upsertPurgeJob");

        try {
            auto jobCollection = Database::instance().collection(PURGE_JOB_COLLECTION);

            job.modified = std::chrono::system_clock::now();
            const auto filter = make_document(kvp("jobId", job.jobId));
            const auto update = make_document(kvp("$set", job.toDocument()));

            mongocxx::options::find_one_and_update opts;
            opts.upsert(true);
            opts.return_document(mongocxx::options::return_document::k_after);

            if (auto result = jobCollection.find_one_and_update(filter.view(), update.view(), opts)) {
                return Entity::ESM::PurgeJob::fromDocument(result->view());
            }
            throw std::runtime_error("upsert returned no document, jobId: " + job.jobId);

        } catch (const std::exception &e) {
            log_error << "Upsert purge job failed, jobId: " << job.jobId << ", error: " << e.what();
            throw;
        }
    }

    std::optional<Entity::ESM::PurgeJob> MongoEsmRepository::claimPurgeJob(const std::string &instanceId, const std::chrono::seconds staleAfter) {

        Core::Monitoring::MonitoringTimer measure(kRepositoryTimer, kRepositoryCounter, "operation", "claimPurgeJob");

        try {
            auto jobCollection = Database::instance().collection(PURGE_JOB_COLLECTION);

            const auto now = std::chrono::system_clock::now();
            const auto cutoff = bsoncxx::types::b_date{std::chrono::duration_cast<std::chrono::milliseconds>((now - staleAfter).time_since_epoch())};

            // Free means nobody holds it, or whoever held it has not finished a page within
            // staleAfter - which is what a worker that was stopped mid-purge leaves behind.
            // Matched and updated in one operation so two instances sweeping in the same second
            // cannot both take it.
            const auto filter = make_document(kvp("$or", bsoncxx::builder::basic::make_array(
                                                                 make_document(kvp("claimedBy", "")),
                                                                 make_document(kvp("claimedAt", make_document(kvp("$lt", cutoff)))))));

            const auto update = make_document(kvp("$set", make_document(
                                                                  kvp("claimedBy", instanceId),
                                                                  kvp("claimedAt", bsoncxx::types::b_date{std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch())}),
                                                                  kvp("modified", bsoncxx::types::b_date{std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch())}))));

            mongocxx::options::find_one_and_update opts;
            opts.return_document(mongocxx::options::return_document::k_after);

            if (auto result = jobCollection.find_one_and_update(filter.view(), update.view(), opts)) {
                return Entity::ESM::PurgeJob::fromDocument(result->view());
            }
            return std::nullopt;

        } catch (const std::exception &e) {
            log_error << "Claim purge job failed, instanceId: " << instanceId << ", error: " << e.what();
            throw;
        }
    }

    bool MongoEsmRepository::heartbeatPurgeJob(const std::string &jobId, const std::string &instanceId,
                                               const long removedObjects, const long removedSize) {

        Core::Monitoring::MonitoringTimer measure(kRepositoryTimer, kRepositoryCounter, "operation", "heartbeatPurgeJob");

        try {
            auto jobCollection = Database::instance().collection(PURGE_JOB_COLLECTION);

            const auto now = bsoncxx::types::b_date{std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())};

            // The instanceId is part of the filter, not just the update: a worker whose claim was
            // taken while it was slow must find out here and stop, rather than carry on removing
            // objects that another worker is also removing and counting.
            const auto filter = make_document(kvp("jobId", jobId), kvp("claimedBy", instanceId));
            const auto update = make_document(
                    kvp("$set", make_document(kvp("claimedAt", now), kvp("modified", now))),
                    kvp("$inc", make_document(
                                        kvp("removedObjects", static_cast<std::int64_t>(removedObjects)),
                                        kvp("removedSize", static_cast<std::int64_t>(removedSize)))));

            const auto result = jobCollection.update_one(filter.view(), update.view());
            return result.has_value() && result->matched_count() > 0;

        } catch (const std::exception &e) {
            log_error << "Heartbeat purge job failed, jobId: " << jobId << ", error: " << e.what();
            return false;
        }
    }

    void MongoEsmRepository::deletePurgeJob(const std::string &jobId) {

        Core::Monitoring::MonitoringTimer measure(kRepositoryTimer, kRepositoryCounter, "operation", "deletePurgeJob");

        try {
            auto jobCollection = Database::instance().collection(PURGE_JOB_COLLECTION);
            std::ignore = jobCollection.delete_one(make_document(kvp("jobId", jobId)).view());

        } catch (const std::exception &e) {
            log_error << "Delete purge job failed, jobId: " << jobId << ", error: " << e.what();
            throw;
        }
    }

    std::vector<Entity::ESM::PurgeJob> MongoEsmRepository::listPurgeJobs() const {

        Core::Monitoring::MonitoringTimer measure(kRepositoryTimer, kRepositoryCounter, "operation", "listPurgeJobs");

        try {
            auto jobCollection = Database::instance().collection(PURGE_JOB_COLLECTION);

            std::vector<Entity::ESM::PurgeJob> jobs;
            for (auto cursor = jobCollection.find({}); const auto &document: cursor) {
                jobs.push_back(Entity::ESM::PurgeJob::fromDocument(document));
            }
            return jobs;

        } catch (const std::exception &e) {
            log_error << "List purge jobs failed, error: " << e.what();
            throw;
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

            auto bucketCollection = Database::instance().collection(BUCKET_COLLECTION);

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

            auto bucketCollection = Database::instance().collection(BUCKET_COLLECTION);

            const int64_t count = bucketCollection.count_documents(filter.extract());
            return static_cast<long>(count);

        } catch (const std::exception &e) {

            log_error << "Count buckets failed, error: " << e.what();
        }
        return -1;
    }

    void MongoEsmRepository::removeBucketByName(const std::string &accountId, const std::string &nameSpace, const std::string &name) {

        try {
            auto bucketCollection = Database::instance().collection(BUCKET_COLLECTION);

            // delete_many on the name alone would have taken every account's bucket of that name
            // with it. The filter matches the unique index, so this can now delete at most the one
            // bucket the caller named.
            const auto filter = make_document(kvp("accountId", accountId), kvp("namespace", nameSpace), kvp("name", name));
            const auto result = bucketCollection.delete_many(filter.view());
            log_debug << "Bucket deleted, count: " << result->deleted_count();

        } catch (const std::exception &e) {
            log_error << "Delete bucket failed, error: " << e.what();
        }
    }

    void MongoEsmRepository::deleteBucketByErn(const std::string &ern) {

        try {
            auto bucketCollection = Database::instance().collection(BUCKET_COLLECTION);

            const auto result = bucketCollection.delete_many(make_document(kvp("ern", ern)));
            log_debug << "Bucket deleted, count: " << result->deleted_count();

        } catch (const std::exception &e) {
            log_error << "Delete bucket failed, error: " << e.what();
        }
    }

    void MongoEsmRepository::clearBuckets() {

        try {
            auto bucketCollection = Database::instance().collection(BUCKET_COLLECTION);

            const auto result = bucketCollection.delete_many({});
            log_debug << "All buckets deleted, count: " << result->deleted_count();

        } catch (const std::exception &e) {
            log_error << "Delete all buckets failed, error: " << e.what();
        }
    }

    Entity::ESM::Object MongoEsmRepository::upsertObject(Entity::ESM::Object &object) {

        // Derived here rather than by each caller, so they cannot disagree with the key they
        // describe - the counters read the first of these and a level listing the second, and
        // neither looks at the key.
        object.directory = Entity::ESM::IsDirectoryKey(object.key);
        object.parent = Entity::ESM::ParentOfKey(object.key);

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

            auto objectCollection = Database::instance().collection(OBJECT_COLLECTION);

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

            auto objectCollection = Database::instance().collection(OBJECT_COLLECTION);

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

            auto objectCollection = Database::instance().collection(OBJECT_COLLECTION);

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
            auto messageCollection = Database::instance().collection(OBJECT_COLLECTION);

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
            auto messageCollection = Database::instance().collection(OBJECT_COLLECTION);

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

            auto objectCollection = Database::instance().collection(OBJECT_COLLECTION);

            return objectCollection.count_documents(filter.view());
        } catch (const std::exception &e) {
            log_error << "Count encrypted objects failed, ern: " << bucketErn << ", error: " << e.what();
        }
        return -1;
    }

    void MongoEsmRepository::ensureLevelIndex() {

        // (bucketErn, parent, directory, key) replaces (bucketErn, parent, key). With "directory"
        // in the index, "this level without its markers" is the equality directory: false - a
        // plain index range, counted and skipped through without a document being read. Filtered
        // as $ne: true against the old index, every object of the level had to be fetched just
        // to look at that one field: 77 seconds to count a 4.1 million object folder, against 4
        // from the index alone.
        //
        // On a thread of its own because on a populated collection the build takes minutes, and
        // this runs while the repository is being constructed - every ESM start would wait for
        // it. Nothing depends on it existing: no query names an index, so until the build is done
        // the planner keeps using the old one, as fast or as slow as before.
        //
        // The in-memory backend has nothing to build from and nothing to drop, so it gets the
        // index inline rather than a thread that would outlive whoever reset the store.
        const auto levelKey = make_document(kvp("bucketErn", 1), kvp("parent", 1), kvp("directory", 1), kvp("key", 1));
        if (!Database::instance().collection(OBJECT_COLLECTION).supports_aggregation()) {
            Database::instance().collection(OBJECT_COLLECTION).create_index(levelKey.view());
            return;
        }

        std::thread([levelKey] {
            try {
                const auto objectCollection = Database::instance().collection(OBJECT_COLLECTION);

                // Returns at once when the index already exists, so only a real build is timed.
                const auto started = std::chrono::steady_clock::now();
                objectCollection.create_index(levelKey.view());
                if (const auto took = std::chrono::steady_clock::now() - started; took > std::chrono::seconds(5)) {
                    log_info << "ESM level index built, seconds: " << std::chrono::duration_cast<std::chrono::seconds>(took).count();
                }

                // Only once its replacement exists, so there is never a moment with neither. Every
                // query the old one served, the new one serves too. Nothing to drop is a no-op.
                objectCollection.drop_index("bucketErn_1_parent_1_key_1");

            } catch (const std::exception &e) {
                log_error << "Ensure ESM level index failed, error: " << e.what();
            }
        }).detach();
    }

    namespace {
        // The first string after every string that starts with prefix, as an exclusive upper bound
        // for a range over them: the last byte incremented, any trailing 0xff bytes dropped first.
        // Empty when there is none (an empty prefix, or nothing but 0xff), meaning "no bound".
        std::string PrefixUpperBound(std::string prefix) {
            while (!prefix.empty() && static_cast<unsigned char>(prefix.back()) == 0xff) prefix.pop_back();
            if (!prefix.empty()) prefix.back() = static_cast<char>(static_cast<unsigned char>(prefix.back()) + 1);
            return prefix;
        }
    }// namespace

    std::vector<Entity::ESM::Object> MongoEsmRepository::listLevel(const std::string &bucketErn, const std::string &parent, const long pageSize, const long pageIndex, const std::string &sortDirection, const bool includeDirectories) const {

        try {
            document filter = {};
            filter.append(kvp("bucketErn", bucketErn), kvp("parent", parent));
            // A marker of this level is a key in its own right and sits here like anything else;
            // whoever manages directories asks for them, and nobody else is shown them.
            //
            // Both as equalities on "directory", never left out: the index has it between parent
            // and key, so only with it pinned does the index deliver key order - two ranges merged
            // when markers are wanted, one when not. Left unconstrained, the level would be read
            // whole and sorted in memory before the first page could be returned.
            if (includeDirectories) {
                filter.append(kvp("directory", make_document(kvp("$in", make_array(false, true)))));
            } else {
                filter.append(kvp("directory", false));
            }

            mongocxx::options::find opts;
            // By key, always: the index is (bucketErn, parent, directory, key), so this sort is
            // read from it rather than performed, and any other column would throw that away.
            opts.sort(make_document(kvp("key", sortDirection == "desc" ? -1 : 1)));
            if (pageSize > 0) {
                opts.limit(pageSize);
                opts.skip(std::max<long>(pageIndex, 0) * pageSize);
            }

            std::vector<Entity::ESM::Object> objects;
            auto objectCollection = Database::instance().collection(OBJECT_COLLECTION);
            for (auto cursor = objectCollection.find(filter.view(), opts); auto object: cursor) {
                objects.push_back(Entity::ESM::Object::fromDocument(object));
            }
            return objects;

        } catch (const std::exception &e) {
            log_error << "List level failed, bucket: " << bucketErn << ", parent: " << parent << ", error: " << e.what();
            return {};
        }
    }

    long MongoEsmRepository::countLevel(const std::string &bucketErn, const std::string &parent, const bool includeDirectories) const {

        try {
            document filter = {};
            filter.append(kvp("bucketErn", bucketErn), kvp("parent", parent));
            // An equality, so the count is read from the index - see listLevel().
            if (!includeDirectories) {
                filter.append(kvp("directory", false));
            }
            return Database::instance().collection(OBJECT_COLLECTION).count_documents(filter.view());

        } catch (const std::exception &e) {
            log_error << "Count level failed, bucket: " << bucketErn << ", parent: " << parent << ", error: " << e.what();
            return -1;
        }
    }

    std::vector<std::string> MongoEsmRepository::listSubdirectories(const std::string &bucketErn, const std::string &parent) const {

        try {
            // A skip scan: one index seek per subdirectory, never a read of what is inside one.
            // Each seek finds the smallest parent after the last subdirectory found; its first
            // segment below this level is the next subdirectory, and the next seek starts past
            // everything under that one - at "<parent><segment>0", "0" being the byte after "/".
            //
            // Grouping every parent below the level instead, as this did before, costs one
            // document per object in the subtree: 221 seconds to find the 7 folders at the root of
            // a bucket of 7.9 million objects, which this finds in 8 seeks and 28 ms. The projection
            // names only "parent", so each seek is answered from the index without a fetch.
            const auto upper = PrefixUpperBound(parent);

            mongocxx::options::find opts;
            opts.projection(make_document(kvp("_id", 0), kvp("parent", 1)));
            opts.sort(make_document(kvp("parent", 1)));
            opts.limit(1);

            std::vector<std::string> directories;
            auto objectCollection = Database::instance().collection(OBJECT_COLLECTION);

            // Strictly after the level itself, whose own objects have exactly it as their parent.
            std::string from = parent;
            bool inclusive = false;
            for (;;) {
                document range = {};
                range.append(kvp(std::string(inclusive ? "$gte" : "$gt"), from));
                if (!upper.empty()) range.append(kvp("$lt", upper));

                auto cursor = objectCollection.find(
                        make_document(kvp("bucketErn", bucketErn), kvp("parent", range.extract())), opts);
                const auto found = cursor.begin();
                if (found == cursor.end()) break;

                const auto element = (*found)["parent"];
                if (!element || element.type() != bsoncxx::type::k_string) break;
                const std::string next(element.get_string().value);
                if (next.size() <= parent.size() || next.compare(0, parent.size(), parent) != 0) break;

                const auto rest = next.substr(parent.size());
                const auto segment = rest.substr(0, rest.find('/'));
                // "a//b" has the parent "a//": an empty segment, which is not a directory of its
                // own. Skipped as the grouping skipped it, and the seek moves past it the same way.
                if (!segment.empty()) directories.push_back(parent + segment + "/");

                from = parent + segment + "0";
                inclusive = true;
            }
            return directories;

        } catch (const std::exception &e) {
            log_error << "List subdirectories failed, bucket: " << bucketErn << ", parent: " << parent << ", error: " << e.what();
            return {};
        }
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
            auto objectCollection = Database::instance().collection(OBJECT_COLLECTION);

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
            auto bucketCollection = Database::instance().collection(OBJECT_COLLECTION);

            const auto result = bucketCollection.delete_many(make_document(kvp("ern", ern)));
            log_debug << "Object deleted, count: " << result->deleted_count();

        } catch (const std::exception &e) {
            log_error << "Delete object failed, error: " << e.what();
        }
    }

    long MongoEsmRepository::deleteObjectsByErns(const std::vector<std::string> &erns) {

        Core::Monitoring::MonitoringTimer measure(kRepositoryTimer, kRepositoryCounter, "operation", "deleteObjectsByErns");

        if (erns.empty()) return 0;

        try {
            auto objectCollection = Database::instance().collection(OBJECT_COLLECTION);

            bsoncxx::builder::basic::array wanted;
            for (const auto &ern: erns) wanted.append(ern);

            // One statement for the whole page, served by the unique index on "ern" exactly as a
            // single-ERN delete is - $in over an indexed field is a series of index lookups inside
            // the server rather than a collection scan, so the saving is the round trips and
            // nothing is paid back in query cost.
            const auto result = objectCollection.delete_many(
                    make_document(kvp("ern", make_document(kvp("$in", wanted)))));
            const long deleted = result ? static_cast<long>(result->deleted_count()) : 0;

            log_debug << "Objects deleted, asked: " << erns.size() << ", deleted: " << deleted;
            return deleted;

        } catch (const std::exception &e) {
            // Swallowed like its single-object counterpart above. A page that will not delete is
            // reported by the caller's own no-progress guard, which stops rather than spinning.
            log_error << "Delete objects failed, count: " << erns.size() << ", error: " << e.what();
            return 0;
        }
    }

    std::optional<Entity::ESM::Bucket> MongoEsmRepository::renameBucket(const std::string &ern, const std::string &newName,
                                                                        const std::string &newErn) {

        try {
            auto bucketCollection = Database::instance().collection(BUCKET_COLLECTION);

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
            auto subscriptionCollection = Database::instance().collection(SUBSCRIPTION_COLLECTION);

            const auto result = subscriptionCollection.update_many(
                    make_document(kvp("sourceErn", oldSourceErn)).view(),
                    make_document(kvp("$set", make_document(kvp("sourceErn", newSourceErn)))).view());

            // After the write - see upsertSubscription.
            subscriptionCache().erase(oldSourceErn);
            subscriptionCache().erase(newSourceErn);
            return result ? static_cast<long>(result->modified_count()) : 0;

        } catch (const std::exception &e) {
            log_error << "Repoint subscriptions failed, error: " << e.what();
            return 0;
        }
    }

    long MongoEsmRepository::renameBucketObjects(const std::string &oldBucketErn, const std::string &newBucketErn,
                                                 const std::string &oldName, const std::string &newName) {

        try {
            auto objectCollection = Database::instance().collection(OBJECT_COLLECTION);

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

            long renamed = 0;
            if (objectCollection.supports_aggregation()) {
                const auto result = objectCollection.update_many(make_document(kvp("bucketErn", oldBucketErn)), pipeline);
                renamed = result ? static_cast<long>(result->modified_count()) : 0;
            } else {
                // The same rewrite, one object at a time, for a backend with no server to evaluate
                // the pipeline. A round trip per object, which is the right trade for a store that
                // holds a test's worth of them.
                for (auto cursor = objectCollection.find(make_document(kvp("bucketErn", oldBucketErn))); auto document: cursor) {

                    const auto ern = std::string(document["ern"].get_string().value);
                    const auto position = ern.find(oldSegment);
                    const auto rewritten = position == std::string::npos
                                                   ? ern
                                                   : ern.substr(0, position) + newSegment + ern.substr(position + oldSegment.size());

                    std::ignore = objectCollection.update_one(
                            make_document(kvp("_id", document["_id"].get_oid().value)),
                            make_document(kvp("$set", make_document(kvp("bucketErn", newBucketErn), kvp("ern", rewritten)))));
                    renamed++;
                }
            }
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

            auto subscriptionCollection = Database::instance().collection(SUBSCRIPTION_COLLECTION);

            const auto result = subscriptionCollection.find_one_and_update(filter.view(), update.view(), opts);

            // After the write, never before: a lookup starting in between would read the old list
            // and store it. One already reading when this runs is not stored either - the cache's
            // generation sees to that.
            subscriptionCache().erase(subscription.sourceErn);

            if (result) {
                return Entity::ESM::Subscription::fromDocument(result->view());
            }
            throw std::runtime_error("upsert returned no document, sourceErn: " + subscription.sourceErn);

        } catch (const std::exception &e) {
            log_error << "Upsert subscription failed, error: " << e.what();
            throw;
        }
    }

    std::optional<std::vector<Entity::ESM::Subscription> > MongoEsmRepository::loadSubscriptions(const std::string &sourceErn) {

        try {
            std::vector<Entity::ESM::Subscription> subscriptions;
            const auto filter = make_document(kvp("sourceErn", sourceErn));

            auto subscriptionCollection = Database::instance().collection(SUBSCRIPTION_COLLECTION);

            for (auto cursor = subscriptionCollection.find(filter.view()); auto doc: cursor) {
                subscriptions.push_back(Entity::ESM::Subscription::fromDocument(doc));
            }
            return subscriptions;

        } catch (const std::exception &e) {
            log_error << "List subscriptions failed, sourceErn: " << sourceErn << ", error: " << e.what();
            return std::nullopt;
        }
    }

    std::vector<Entity::ESM::Subscription> MongoEsmRepository::listSubscriptionsBySourceErn(const std::string &sourceErn) const {
        return loadSubscriptions(sourceErn).value_or(std::vector<Entity::ESM::Subscription>{});
    }

    void MongoEsmRepository::deleteSubscriptionByErn(const std::string &ern) {

        try {
            auto subscriptionCollection = Database::instance().collection(SUBSCRIPTION_COLLECTION);

            const auto result = subscriptionCollection.delete_many(make_document(kvp("ern", ern)));

            // All of them, after the write - see upsertSubscription. Knowing only the subscription's
            // ERN there is no telling which bucket's entry it was in, and subscriptions change
            // rarely enough that forgetting every bucket costs one read each.
            subscriptionCache().clear();
            log_debug << "Subscription deleted, ern: " << ern << ", count: " << result->deleted_count();

        } catch (const std::exception &e) {
            log_error << "Delete subscription failed, ern: " << ern << ", error: " << e.what();
        }
    }

    void MongoEsmRepository::recountBuckets() {

        Core::Monitoring::MonitoringTimer measure(kRepositoryTimer, kRepositoryCounter, "operation", "recount-buckets");

        try {
            auto objectCollection = Database::instance().collection(OBJECT_COLLECTION);
            auto bucketCollection = Database::instance().collection(BUCKET_COLLECTION);

            // One pass over the objects, with the accumulators the loop below actually reads.
            //
            // What is counted has to match what put-object and delete-object maintain incrementally,
            // or the recount would "correct" the counters to a different definition every time it
            // ran: every object contributes its bytes, but a directory marker (a zero-byte key
            // ending in "/") is not counted as an object, exactly as EsmServer does when it stores
            // one. Only COMPLETED objects count - a multipart upload in progress has rows that no
            // bucket counter has been told about yet.
            // Grouped, counted and summed through the neutral path, so a recount works with or
            // without a server to evaluate a pipeline on - see Collection::group_count.
            struct Counts {
                long count{};
                long size{};
            };
            std::unordered_map<std::string, Counts> counted;

            // Filtered, and this is the whole point of the pass. It used to group the object
            // collection with an EMPTY filter - every document, directory markers and in-flight
            // uploads included - directly contradicting the paragraph above and the incremental
            // path it claims to match. Since EMO runs this several times a minute, the wrong
            // figure did not merely appear once: it was restored over the right one, so no
            // correction to a bucket's counters could survive a minute. Seen as a transfer bucket
            // reporting 5 objects for 1 file and 4 directory markers, permanently.
            const auto completedFiles = make_document(
                    kvp("status", std::string(Entity::ESM::ObjectStatusToString(Entity::ESM::ObjectStatus::COMPLETED))),
                    kvp("directory", make_document(kvp("$ne", true))));

            for (const auto &group: objectCollection.group_count(completedFiles.view(), {"bucketErn"}, "size")) {
                counted[group.key[0]] = {.count = group.count, .size = group.sum};
            }

            // Directory markers, counted on their own so the two can be told apart rather than one
            // hidden inside the other. Their size is deliberately not summed - a marker is zero
            // bytes, and a bucket's size is the bytes a client would get back out of it.
            const auto directories = make_document(kvp("directory", true));
            std::unordered_map<std::string, long> countedDirectories;
            for (const auto &group: objectCollection.group_count(directories.view(), {"bucketErn"})) {
                countedDirectories[group.key[0]] = group.count;
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
                const auto dir = countedDirectories.find(ern);
                const long directoryCount = dir != countedDirectories.end() ? dir->second : 0;
                bucketCollection.update_one(make_document(kvp("ern", ern)).view(),
                                            make_document(kvp("$set", make_document(
                                                                      kvp("objects", static_cast<int64_t>(counts.count)),
                                                                      kvp("directories", static_cast<int64_t>(directoryCount)),
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