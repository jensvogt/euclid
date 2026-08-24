//
// Created by vogje01 on 8/20/26.
//

#pragma once

// C++ standard includes
#include <string>

// Euclid includes
#include <euclid/core/LogStream.h>
#include <euclid/database/Database.h>
#include <euclid/database/repository/esm/IEsmRepository.h>

namespace Euclid::Database {

    using namespace bsoncxx::builder::basic;

    /**
     * @brief Storage MongoDB database.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class MongoEsmRepository final : public IEsmRepository {

    public:

        /**
         * @brief Constructor
         */
        explicit MongoEsmRepository();

        /**
         * @brief Singleton instance
         */
        static MongoEsmRepository &instance() {
            static MongoEsmRepository storageDatabase;
            return storageDatabase;
        }

        /**
         * @brief Update or insert a bucket
         *
         * @param bucket bucket entity
         */
        Entity::ESM::Bucket upsertBucket(Entity::ESM::Bucket &bucket) override;

        /**
         * @brief Removes a bucket entity by name
         *
         * @param name bucket name
         */
        void removeBucketByName(const std::string &name) override;

        /**
         * @brief Removes a bucket entity by ERN
         *
         * @param ern bucket ERN
         */
        void deleteBucketByErn(const std::string &ern) override;

        /**
         * @brief Find by bucket name
         *
         * @param name bucket name
         * @return optional bucket
         */
        [[nodiscard]]
        std::optional<Entity::ESM::Bucket> findBucketByName(const std::string &name) const override;

        /**
         * @brief Find by bucket ID
         *
         * @param oid bucket OID
         * @return optional bucket
         */
        [[nodiscard]]
        std::optional<Entity::ESM::Bucket> findBucketById(const std::string &oid) const override;

        /**
         * @brief Find by bucket ERN
         *
         * @param ern bucket ERN
         * @return optional bucket
         */
        [[nodiscard]]
        std::optional<Entity::ESM::Bucket> findBucketByErn(const std::string &ern) const override;

        /**
         * @brief Find all buckets, optionally filtered, paged and sorted.
         *
         * @param prefix only buckets whose name starts with this prefix are returned; empty matches all buckets
         * @param pageSize maximum number of buckets to return; 0 or less means no limit
         * @param pageIndex zero-based page index, applied when pageSize is set
         * @param sortColumn field to sort by (e.g. "name", "ern"); empty means unsorted
         * @return list of matching bucket entities.
         */
        [[nodiscard]]
        std::vector<Entity::ESM::Bucket> listBuckets(const std::string &prefix, long pageSize, long pageIndex, const std::string &sortColumn) const override;

        /**
         * @brief Check the existence of a bucket by name
         *
         * @param name bucket name to check existence
         * @return true if the bucket exists
         */
        [[nodiscard]]
        bool bucketExists(const std::string &name) const override;

        /**
         * @brief Get the total number of buckets
         *
         * @return total number of buckets
         */
        [[nodiscard]]
        long countBuckets() const override;

        /**
         * @brief Delete all buckets
         */
        void clearBuckets() override;

        /**
         * @brief Update or insert an object, keyed by bucketErn+key
         *
         * @param object object entity
         */
        Entity::ESM::Object upsertObject(Entity::ESM::Object &object) override;

        /**
         * @brief Find an object by its bucket and key
         *
         * @param bucketErn ERN of the bucket the object belongs to
         * @param key object key within the bucket
         * @return optional object
         */
        [[nodiscard]]
        std::optional<Entity::ESM::Object> findObjectByBucketAndKey(const std::string &bucketErn, const std::string &key) const override;

        /**
         * @brief Find an object by its ERN
         *
         * @param ern ERN of the object
         * @return optional object
         */
        [[nodiscard]]
        std::optional<Entity::ESM::Object> findObjectByErn(const std::string &ern) const override;

        /**
         * @brief Retrieves the total number of objects in the repository.
         *
         * @return The total number of objects as a long integer.
         */
        [[nodiscard]]
        long countObjects() const override;

        /**
         * @brief Retrieves the total number og objects in a bucket.
         *
         * @paranm bucketErn bucket ERN
         * @return The total number of messages as a long integer.
         */
        [[nodiscard]]
        long countObjects(const std::string &bucketErn) const override;

        /**
         * @brief Find all buckets, optionally filtered, paged and sorted.
         *
         * @param bucketErn ERN of the bucket the object belongs to
         * @param prefix only buckets whose name starts with this prefix are returned; empty matches all buckets
         * @param pageSize maximum number of buckets to return; 0 or less means no limit
         * @param pageIndex zero-based page index, applied when pageSize is set
         * @param sortColumn field to sort by (e.g. "name", "ern"); empty means unsorted
         * @return list of matching bucket entities.
         */
        [[nodiscard]]
        std::vector<Entity::ESM::Object> listObjects(const std::string &bucketErn, const std::string &prefix, long pageSize, long pageIndex, const std::string &sortColumn) const override;

        /**
         * @brief Removes a object entity by ERN
         *
         * @param ern object ERN
         */
        void deleteObjectByErn(const std::string &ern) override;

    private:

        static constexpr auto BUCKET_COLLECTION = "storage_bucket";
        static constexpr auto OBJECT_COLLECTION = "storage_object";

        /**
         * @brief Creates the indexes required for efficient bucket lookup, if they do not already exist.
         */
        static void ensureIndexes();

        /**
         * Database name
         */
        std::string _databaseName;
    };

}// namespace Euclid::Database