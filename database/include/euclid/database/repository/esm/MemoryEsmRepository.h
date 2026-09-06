//
// Created by vogje01 on 8/20/26.
//
#pragma once

// C++ includes
#include <algorithm>
#include <mutex>
#include <ranges>
#include <unordered_map>

// Euclid includes
#include <euclid/core/UuidUtils.h>
#include <euclid/database/entity/esm/Bucket.h>
#include <euclid/database/entity/esm/Subscription.h>
#include <euclid/database/repository/esm/IEsmRepository.h>

namespace Euclid::Database {

    /**
     * @brief Storage memory database.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class MemoryEsmRepository final : public IEsmRepository {

    public:

        /**
         * @brief Singleton instance
         */
        static MemoryEsmRepository &instance() {
            static MemoryEsmRepository storageDatabase;
            return storageDatabase;
        }

        /**
         * @brief Update or insert a bucket
         *
         * @param bucket bucket entity
         */
        Entity::ESM::Bucket upsertBucket(Entity::ESM::Bucket &bucket) override {
            std::lock_guard lock(_mutex);
            if (bucket.oid.empty()) {
                bucket.oid = Core::UuidUtils::CreateRandomUuid();
            }
            _bucketStore[bucket.oid] = bucket;
            return bucket;
        }

        /**
         * @brief Removes a bucket entity by name
         *
         * @param name bucket name
         */
        void removeBucketByName(const std::string &name) override {
            std::lock_guard lock(_mutex);
            std::erase_if(_bucketStore, [&name](const auto &kv) {
                return kv.second.name == name;
            });
        }

        /**
         * @brief Removes a bucket entity by ERN
         *
         * @param ern bucket ERN
         */
        void deleteBucketByErn(const std::string &ern) override {
            std::lock_guard lock(_mutex);
            std::erase_if(_bucketStore, [&ern](const auto &kv) {
                return kv.second.ern == ern;
            });
        }

        /**
         * @brief Removes a bucket entity by name
         *
         * @param name bucket name
         */
        std::optional<Entity::ESM::Bucket> findBucketByName(const std::string &name) const override {
            std::lock_guard lock(_mutex);
            for (const auto &b: _bucketStore | std::views::values) {
                if (b.name == name) return b;
            }
            return std::nullopt;
        }

        /**
         * @brief Find by bucket ID
         *
         * @param id bucket ID
         * @return optional bucket
         */
        std::optional<Entity::ESM::Bucket> findBucketById(const std::string &id) const override {
            std::lock_guard lock(_mutex);
            for (const auto &b: _bucketStore | std::views::values) {
                if (b.oid == id) return b;
            }
            return std::nullopt;
        }

        /**
         * @brief Find by bucket ERN
         *
         * @param ern bucket ERN
         * @return optional bucket
         */
        std::optional<Entity::ESM::Bucket> findBucketByErn(const std::string &ern) const override {
            std::lock_guard lock(_mutex);
            for (const auto &b: _bucketStore | std::views::values) {
                if (b.ern == ern) return b;
            }
            return std::nullopt;
        }

        /**
         * @brief Find all buckets, optionally filtered, paged and sorted.
         *
         * @param prefix only buckets whose name starts with this prefix are returned; empty matches all buckets
         * @param pageSize maximum number of buckets to return; 0 or less means no limit
         * @param pageIndex zero-based page index, applied when pageSize is set
         * @param sortColumn field to sort by (e.g. "name", "ern"); empty means unsorted
         * @return list of matching bucket entities.
         */
        std::vector<Entity::ESM::Bucket> listBuckets(const std::string &accountId, const std::string &namespaceName, const std::string &prefix, const long pageSize, const long pageIndex, const std::string &sortColumn, const std::string &sortDirection, const bool includeInternal = false) const override {
            std::lock_guard lock(_mutex);
            std::vector<Entity::ESM::Bucket> result;
            for (const auto &b: _bucketStore | std::views::values) {
                if (b.accountId != accountId) continue;
                if (b.internal && !includeInternal) continue;
                if (!namespaceName.empty() && b.nameSpace != namespaceName) continue;
                if (prefix.empty() || b.name.starts_with(prefix)) {
                    result.push_back(b);
                }
            }

            if (sortColumn == "name") {
                std::ranges::sort(result, {}, &Entity::ESM::Bucket::name);
            } else if (sortColumn == "ern") {
                std::ranges::sort(result, {}, &Entity::ESM::Bucket::ern);
            }

            if (pageSize > 0) {
                const auto offset = std::min<size_t>(std::max<long>(pageIndex, 0) * pageSize, result.size());
                const auto end = std::min<size_t>(offset + pageSize, result.size());
                result = std::vector(result.begin() + static_cast<long>(offset), result.begin() + static_cast<long>(end));
            }
            return result;
        }

        /**
         * @brief Check the existence of a bucket by name
         *
         * @param name bucket name to check existence
         * @return true if the bucket exists
         */
        bool bucketExists(const std::string &name) const override {
            std::lock_guard lock(_mutex);
            return std::ranges::any_of(_bucketStore | std::views::values, [&name](const auto &b) {
                return b.name == name;
            });
        }

        /**
         * @brief Get the total number of buckets
         *
         * @return total number of buckets
         */
        long countBuckets(const std::string &accountId, const std::string &namespaceName, const std::string &prefix, const bool includeInternal = false) const override {
            std::lock_guard lock(_mutex);
            return std::ranges::count_if(_bucketStore | std::views::values, [&](const auto &b) {
                return b.accountId == accountId
                       && (includeInternal || !b.internal)
                       && (namespaceName.empty() || b.nameSpace == namespaceName)
                       && (prefix.empty() || b.name.starts_with(prefix));
            });
        }

        void recountBuckets() override {
            std::lock_guard lock(_mutex);
            for (auto &queue: _bucketStore | std::views::values) {
                long count = 0, size = 0;
                for (const auto &object: _objectStore | std::views::values) {
                    if (object.bucketErn != queue.ern) continue;
                    size += object.size;
                    count++;
                }
                queue.objects = count;
                queue.size = size;
            }
        }

        /**
         * @brief Delete all buckets
         */
        void clearBuckets() override {
            std::lock_guard lock(_mutex);
            _bucketStore.clear();
        }

        /**
         * @brief Update or insert an object, keyed by bucketErn+key
         *
         * @param object object entity
         */
        Entity::ESM::Object upsertObject(Entity::ESM::Object &object) override {
            object.directory = Entity::ESM::IsDirectoryKey(object.key);
            std::lock_guard lock(_mutex);
            if (object.oid.empty()) {
                object.oid = Core::UuidUtils::CreateRandomUuid();
            }
            _objectStore[object.oid] = object;
            return object;
        }

        /**
         * @brief Find an object by its bucket and key
         *
         * @param bucketErn ERN of the bucket the object belongs to
         * @param key object key within the bucket
         * @return optional object
         */
        std::optional<Entity::ESM::Object> findObjectByBucketAndKey(const std::string &bucketErn, const std::string &key) const override {
            std::lock_guard lock(_mutex);
            for (const auto &o: _objectStore | std::views::values) {
                if (o.bucketErn == bucketErn && o.key == key) return o;
            }
            return std::nullopt;
        }

        /**
         * @brief Find an object by its ERN
         *
         * @param ern ERN of the object
         * @return optional object
         */
        std::optional<Entity::ESM::Object> findObjectByErn(const std::string &ern) const override {
            std::lock_guard lock(_mutex);
            for (const auto &o: _objectStore | std::views::values) {
                if (o.ern == ern) return o;
            }
            return std::nullopt;
        }

        /**
         * @brief Retrieves the total number of objects in the repository.
         *
         * @return The total number of objects as a long integer.
         */
        long countObjects() const override {
            std::lock_guard lock(_mutex);
            return static_cast<long>(_objectStore.size());
        }

        /**
         * @brief Retrieves the total number of objects in a bucket, optionally filtered by object key prefix.
         *
         * @paranm bucketErn bucket ERN
         * @paranm prefix object key prefix
         * @param includeDirectories whether directory keys are counted
         * @return The total number of messages as a long integer.
         */
        long countObjects(const std::string &bucketErn, const std::string &prefix, const bool includeDirectories) const override {
            std::lock_guard lock(_mutex);
            return std::ranges::count_if(_objectStore | std::views::values, [&bucketErn,prefix,includeDirectories](const auto &objects) {
                return objects.bucketErn == bucketErn && (prefix.empty() || objects.key.starts_with(prefix)) && (includeDirectories || !Entity::ESM::IsDirectoryKey(objects.key));
            });
        }

        /**
         * @brief Retrieves the number of objects in a bucket whose bytes are stored encrypted.
         *
         * @param bucketErn bucket ERN
         * @return the number of encrypted objects.
         */
        long countEncryptedObjects(const std::string &bucketErn) const override {
            std::lock_guard lock(_mutex);
            return std::ranges::count_if(_objectStore | std::views::values, [&bucketErn](const auto &object) {
                return object.bucketErn == bucketErn && !object.encryptionKeyErn.empty();
            });
        }

        /**
         * @brief Find all buckets, optionally filtered, paged and sorted.
         *
         * @param bucketErn ERN of the bucket the object belongs to
         * @param prefix only buckets whose name starts with this prefix are returned; empty matches all buckets
         * @param pageSize maximum number of buckets to return; 0 or less means no limit
         * @param pageIndex zero-based page index, applied when pageSize is set
         * @param sortColumn field to sort by (e.g. "name", "ern"); empty means unsorted
         * @param sortDirection "asc" or "desc"; anything else is treated as "desc"
         * @param includeDirectories whether directory keys are returned
         * @return list of matching bucket entities.
         */
        std::vector<Entity::ESM::Object> listObjects(const std::string &bucketErn, const std::string &prefix, const long pageSize, const long pageIndex, const std::string &sortColumn, const std::string &sortDirection, const bool includeDirectories) const override {
            std::lock_guard lock(_mutex);
            std::vector<Entity::ESM::Object> result;
            for (const auto &b: _objectStore | std::views::values) {
                if (b.bucketErn == bucketErn && (prefix.empty() || b.key.starts_with(prefix)) && (includeDirectories || !Entity::ESM::IsDirectoryKey(b.key))) {
                    result.push_back(b);
                }
            }

            if (sortColumn == "key") {
                std::ranges::sort(result, {}, &Entity::ESM::Object::key);
            } else if (sortColumn == "ern") {
                std::ranges::sort(result, {}, &Entity::ESM::Object::ern);
            }

            if (pageSize > 0) {
                const auto offset = std::min<size_t>(std::max<long>(pageIndex, 0) * pageSize, result.size());
                const auto end = std::min<size_t>(offset + pageSize, result.size());
                result = std::vector(result.begin() + static_cast<long>(offset), result.begin() + static_cast<long>(end));
            }
            return result;
        }

        /**
         * @brief Removes a object entity by ERN
         *
         * @param ern object ERN
         */
        void deleteObjectByErn(const std::string &ern) override {
            std::lock_guard lock(_mutex);
            std::erase_if(_objectStore, [&ern](const auto &kv) {
                return kv.second.ern == ern;
            });
        }

        std::optional<Entity::ESM::Bucket> renameBucket(const std::string &ern, const std::string &newName,
                                                        const std::string &newErn) override {
            std::lock_guard lock(_mutex);
            for (auto &bucket: _bucketStore | std::views::values) {
                if (bucket.ern != ern) continue;
                bucket.name = newName;
                bucket.ern = newErn;
                return bucket;
            }
            return std::nullopt;
        }

        long repointSubscriptions(const std::string &oldSourceErn, const std::string &newSourceErn) override {
            std::lock_guard lock(_mutex);
            long repointed = 0;
            for (auto &subscription: _subscriptionStore | std::views::values) {
                if (subscription.sourceErn != oldSourceErn) continue;
                subscription.sourceErn = newSourceErn;
                ++repointed;
            }
            return repointed;
        }

        long renameBucketObjects(const std::string &oldBucketErn, const std::string &newBucketErn,
                                 const std::string &oldName, const std::string &newName) override {
            std::lock_guard lock(_mutex);

            // Only the bucket segment of an object's ERN changes; the key that follows it is the
            // object's own and is left exactly as it was, spelling and all.
            const auto oldSegment = ":object:" + oldName + "/";
            const auto newSegment = ":object:" + newName + "/";

            long renamed = 0;
            for (auto &object: _objectStore | std::views::values) {
                if (object.bucketErn != oldBucketErn) continue;
                object.bucketErn = newBucketErn;
                if (const auto position = object.ern.find(oldSegment); position != std::string::npos) {
                    object.ern.replace(position, oldSegment.size(), newSegment);
                }
                ++renamed;
            }
            return renamed;
        }

        Entity::ESM::Subscription upsertSubscription(Entity::ESM::Subscription &subscription) override {
            std::lock_guard lock(_mutex);
            bool found = false;
            for (auto &existing: _subscriptionStore | std::views::values) {
                if (existing.sourceErn == subscription.sourceErn && existing.type == subscription.type && existing.targetErn == subscription.targetErn) {
                    subscription.oid = existing.oid;
                    subscription.ern = existing.ern;
                    subscription.created = existing.created;
                    found = true;
                    break;
                }
            }
            if (!found && subscription.oid.empty()) {
                subscription.oid = Core::UuidUtils::CreateRandomUuid();
            }
            subscription.modified = std::chrono::system_clock::now();
            _subscriptionStore[subscription.oid] = subscription;
            return subscription;
        }

        std::vector<Entity::ESM::Subscription> listSubscriptionsBySourceErn(const std::string &sourceErn) const override {
            std::lock_guard lock(_mutex);
            std::vector<Entity::ESM::Subscription> result;
            for (const auto &s: _subscriptionStore | std::views::values) {
                if (s.sourceErn == sourceErn) result.push_back(s);
            }
            return result;
        }

        void deleteSubscriptionByErn(const std::string &ern) override {
            std::lock_guard lock(_mutex);
            std::erase_if(_subscriptionStore, [&ern](const auto &kv) {
                return kv.second.ern == ern;
            });
        }

    private:

        /**
         * @brief Lock guard
         */
        mutable std::mutex _mutex;

        /**
         * @brief Bucket store
         */
        std::unordered_map<std::string, Entity::ESM::Bucket> _bucketStore;

        /**
         * @brief Object store
         */
        std::unordered_map<std::string, Entity::ESM::Object> _objectStore;

        /**
         * @brief Subscription store
         */
        std::unordered_map<std::string, Entity::ESM::Subscription> _subscriptionStore;
    };

}// namespace Euclid::Database