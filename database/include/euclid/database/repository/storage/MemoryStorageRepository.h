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
#include <euclid/database/entity/storage/Bucket.h>
#include <euclid/database/repository/storage/IStorageRepository.h>

namespace Euclid::Database {

    /**
     * @brief Storage memory database.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class MemoryStorageRepository final : public IStorageRepository {

    public:

        /**
         * @brief Singleton instance
         */
        static MemoryStorageRepository &instance() {
            static MemoryStorageRepository storageDatabase;
            return storageDatabase;
        }

        Entity::Storage::Bucket upsertBucket(Entity::Storage::Bucket &bucket) override {
            std::lock_guard lock(_mutex);
            if (bucket.oid.empty()) {
                bucket.oid = Core::UuidUtils::CreateRandomUuid();
            }
            _bucketStore[bucket.oid] = bucket;
            return bucket;
        }

        void removeBucketByName(const std::string &name) override {
            std::lock_guard lock(_mutex);
            std::erase_if(_bucketStore, [&name](const auto &kv) {
                return kv.second.name == name;
            });
        }

        void deleteBucketByErn(const std::string &ern) override {
            std::lock_guard lock(_mutex);
            std::erase_if(_bucketStore, [&ern](const auto &kv) {
                return kv.second.ern == ern;
            });
        }

        std::optional<Entity::Storage::Bucket> findBucketByName(const std::string &name) const override {
            std::lock_guard lock(_mutex);
            for (const auto &b: _bucketStore | std::views::values) {
                if (b.name == name) return b;
            }
            return std::nullopt;
        }

        std::optional<Entity::Storage::Bucket> findBucketById(const std::string &id) const override {
            std::lock_guard lock(_mutex);
            for (const auto &b: _bucketStore | std::views::values) {
                if (b.oid == id) return b;
            }
            return std::nullopt;
        }

        std::optional<Entity::Storage::Bucket> findBucketByErn(const std::string &ern) const override {
            std::lock_guard lock(_mutex);
            for (const auto &b: _bucketStore | std::views::values) {
                if (b.ern == ern) return b;
            }
            return std::nullopt;
        }

        std::vector<Entity::Storage::Bucket> listBuckets(const std::string &prefix, const long pageSize, const long pageIndex, const std::string &sortColumn) const override {
            std::lock_guard lock(_mutex);
            std::vector<Entity::Storage::Bucket> result;
            for (const auto &b: _bucketStore | std::views::values) {
                if (prefix.empty() || b.name.starts_with(prefix)) {
                    result.push_back(b);
                }
            }

            if (sortColumn == "name") {
                std::ranges::sort(result, {}, &Entity::Storage::Bucket::name);
            } else if (sortColumn == "ern") {
                std::ranges::sort(result, {}, &Entity::Storage::Bucket::ern);
            }

            if (pageSize > 0) {
                const auto offset = std::min<size_t>(std::max<long>(pageIndex, 0) * pageSize, result.size());
                const auto end = std::min<size_t>(offset + pageSize, result.size());
                result = std::vector(result.begin() + static_cast<long>(offset), result.begin() + static_cast<long>(end));
            }
            return result;
        }

        bool bucketExists(const std::string &name) const override {
            std::lock_guard lock(_mutex);
            return std::ranges::any_of(_bucketStore | std::views::values, [&name](const auto &b) {
                return b.name == name;
            });
        }

        long countBuckets() const override {
            std::lock_guard lock(_mutex);
            return static_cast<long>(_bucketStore.size());
        }

        void clearBuckets() override {
            std::lock_guard lock(_mutex);
            _bucketStore.clear();
        }

        Entity::Storage::Object upsertObject(Entity::Storage::Object &object) override {
            std::lock_guard lock(_mutex);
            if (object.oid.empty()) {
                object.oid = Core::UuidUtils::CreateRandomUuid();
            }
            _objectStore[object.oid] = object;
            return object;
        }

        std::optional<Entity::Storage::Object> findObjectByBucketAndKey(const std::string &bucketErn, const std::string &key) const override {
            std::lock_guard lock(_mutex);
            for (const auto &o: _objectStore | std::views::values) {
                if (o.bucketErn == bucketErn && o.key == key) return o;
            }
            return std::nullopt;
        }

        std::optional<Entity::Storage::Object> findObjectByErn(const std::string &ern) const override {
            std::lock_guard lock(_mutex);
            for (const auto &o: _objectStore | std::views::values) {
                if (o.ern == ern) return o;
            }
            return std::nullopt;
        }

        long countObjects() const override {
            std::lock_guard lock(_mutex);
            return _objectStore.size();
        }

        long countObjects(const std::string &bucketErn) const override {
            std::lock_guard lock(_mutex);
            return std::ranges::count_if(_objectStore | std::views::values, [&bucketErn](const auto &message) {
                return message.bucketErn == bucketErn;
            });
        }

        std::vector<Entity::Storage::Object> listObjects(const std::string &bucket, const std::string &prefix, const long pageSize, const long pageIndex, const std::string &sortColumn) const override {
            std::lock_guard lock(_mutex);
            std::vector<Entity::Storage::Object> result;
            for (const auto &b: _objectStore | std::views::values) {
                if (b.bucketErn == bucket && (prefix.empty() || b.key.starts_with(prefix))) {
                    result.push_back(b);
                }
            }

            if (sortColumn == "key") {
                std::ranges::sort(result, {}, &Entity::Storage::Object::key);
            } else if (sortColumn == "ern") {
                std::ranges::sort(result, {}, &Entity::Storage::Object::ern);
            }

            if (pageSize > 0) {
                const auto offset = std::min<size_t>(std::max<long>(pageIndex, 0) * pageSize, result.size());
                const auto end = std::min<size_t>(offset + pageSize, result.size());
                result = std::vector(result.begin() + static_cast<long>(offset), result.begin() + static_cast<long>(end));
            }
            return result;
        }

        void deleteObjectByErn(const std::string &ern) override {
            std::lock_guard lock(_mutex);
            std::erase_if(_objectStore, [&ern](const auto &kv) {
                return kv.second.ern == ern;
            });
        }

    private:

        mutable std::mutex _mutex;
        std::unordered_map<std::string, Entity::Storage::Bucket> _bucketStore;
        std::unordered_map<std::string, Entity::Storage::Object> _objectStore;
    };

}// namespace Euclid::Database