//
// Created by vogje01 on 8/20/26.
//

#pragma once

// C++ standard includes
#include <string>

// Euclid includes
#include <euclid/core/LogStream.h>
#include <euclid/database/Database.h>
#include <euclid/database/repository/storage/IStorageRepository.h>

namespace Euclid::Database {

    using namespace bsoncxx::builder::basic;

    /**
     * @brief Storage MongoDB database.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class MongoStorageRepository final : public IStorageRepository {

    public:

        /**
         * @brief Constructor
         */
        explicit MongoStorageRepository();

        /**
         * @brief Singleton instance
         */
        static MongoStorageRepository &instance() {
            static MongoStorageRepository storageDatabase;
            return storageDatabase;
        }

        /**
         * @brief Update or insert a bucket
         *
         * @param bucket bucket entity
         */
        Entity::Storage::Bucket upsertBucket(Entity::Storage::Bucket &bucket) override;

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
        std::optional<Entity::Storage::Bucket> findBucketByName(const std::string &name) const override;

        /**
         * @brief Find by bucket ID
         *
         * @param oid bucket OID
         * @return optional bucket
         */
        [[nodiscard]]
        std::optional<Entity::Storage::Bucket> findBucketById(const std::string &oid) const override;

        /**
         * @brief Find by bucket ERN
         *
         * @param ern bucket ERN
         * @return optional bucket
         */
        [[nodiscard]]
        std::optional<Entity::Storage::Bucket> findBucketByErn(const std::string &ern) const override;

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
        std::vector<Entity::Storage::Bucket> listBuckets(const std::string &prefix, long pageSize, long pageIndex, const std::string &sortColumn) const override;

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

    private:

        static constexpr auto BUCKET_COLLECTION = "storage_bucket";

        /**
         * @brief Creates the indexes required for efficient bucket lookup, if they do not already exist.
         */
        void ensureIndexes() const;

        /**
         * Database name
         */
        std::string _databaseName;
    };

}// namespace Euclid::Database
