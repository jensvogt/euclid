//
// Created by vogje01 on 8/20/26.
//

#pragma once

// C++ includes
#include <optional>
#include <string>
#include <vector>

// Euclid includes
#include <euclid/database/entity/esm/Bucket.h>
#include <euclid/database/entity/esm/Object.h>

namespace Euclid::Database {

    /**
     * @brief Interface for storage (bucket) repository operations.
     *
     * Provides an abstraction for storing, retrieving, and managing bucket data.
     */
    class IEsmRepository {

    public:

        virtual ~IEsmRepository() = default;

        /**
         * @brief Inserts a new bucket or updates an existing one in the repository.
         *
         * @param bucket The bucket to be inserted or updated in the repository.
         * @return the persisted bucket entity.
         */
        virtual Entity::ESM::Bucket upsertBucket(Entity::ESM::Bucket &bucket) = 0;

        /**
         * @brief Removes a bucket by its name.
         *
         * @param name The name of the bucket to be removed.
         */
        virtual void removeBucketByName(const std::string &name) = 0;

        /**
         * @brief Removes a bucket by its ERN.
         *
         * @param ern The Euclid resource name (ERN) of the bucket to be removed.
         */
        virtual void deleteBucketByErn(const std::string &ern) = 0;

        /**
         * @brief Searches for a bucket by its name.
         *
         * @param name The name of the bucket to search for.
         * @return The matching bucket, or an empty optional if no match is found.
         */
        [[nodiscard]]
        virtual std::optional<Entity::ESM::Bucket> findBucketByName(const std::string &name) const = 0;

        /**
         * @brief Locates a bucket in the repository by its unique identifier.
         *
         * @param oid The unique identifier of the bucket to search for.
         * @return An optional containing the found bucket, or an empty optional
         *         if no bucket with the given identifier exists.
         */
        [[nodiscard]]
        virtual std::optional<Entity::ESM::Bucket> findBucketById(const std::string &oid) const = 0;

        /**
         * @brief Searches for a bucket by its ERN.
         *
         * @param ern The Euclid resource name (ERN) of the bucket to search for.
         * @return The matching bucket, or an empty optional if no match is found.
         */
        [[nodiscard]]
        virtual std::optional<Entity::ESM::Bucket> findBucketByErn(const std::string &ern) const = 0;

        /**
         * @brief Finds and retrieves all buckets, optionally filtered, paged and sorted.
         *
         * @param accountId only buckets belonging to this account are returned
         * @param namespaceName only buckets in this namespace are returned; empty means don't filter by namespace
         * @param prefix only buckets whose name starts with this prefix are returned; empty matches all buckets
         * @param pageSize maximum number of buckets to return; 0 or less means no limit
         * @param pageIndex zero-based page index, applied when pageSize is set
         * @param sortColumn field to sort by (e.g. "name", "ern"); empty means unsorted
         * @return matching, paged and sorted list of buckets
         */
        [[nodiscard]]
        virtual std::vector<Entity::ESM::Bucket> listBuckets(const std::string &accountId, const std::string &namespaceName, const std::string &prefix, long pageSize, long pageIndex, const std::string &sortColumn) const = 0;

        /**
         * @brief Checks if a bucket with the specified name exists in the repository.
         *
         * @param name The name of the bucket to check for existence.
         * @return True if a bucket with the given name exists, otherwise false.
         */
        [[nodiscard]]
        virtual bool bucketExists(const std::string &name) const = 0;

        /**
         * @brief Retrieves the total count of buckets in the repository.
         *
         * @param accountId only buckets belonging to this account are counted
         * @param namespaceName only buckets in this namespace are counted; empty means don't filter by namespace
         * @return The total number of buckets as a long integer.
         */
        [[nodiscard]]
        virtual long countBuckets(const std::string &accountId, const std::string &namespaceName) const = 0;

        /**
         * @brief Removes all entries from the bucket repository, leaving it in an empty state.
         */
        virtual void clearBuckets() = 0;

        /**
         * @brief Inserts a new object or updates the existing one for the same bucket/key.
         *
         * The object's bytes live in a flat file on disk named after its internalName (a UUID);
         * this is the only place the key-to-internalName mapping is recorded, so this call is
         * what makes an uploaded object resolvable by key afterwards.
         *
         * @param object The object to be inserted or updated in the repository.
         * @return the persisted object entity.
         */
        virtual Entity::ESM::Object upsertObject(Entity::ESM::Object &object) = 0;

        /**
         * @brief Searches for an object by its bucket and key.
         *
         * @param bucketErn The ERN of the bucket the object belongs to.
         * @param key The object's key (path) within the bucket.
         * @return The matching object, or an empty optional if no match is found.
         */
        [[nodiscard]]
        virtual std::optional<Entity::ESM::Object> findObjectByBucketAndKey(const std::string &bucketErn, const std::string &key) const = 0;

        /**
         * @brief Searches for an object by its ERN.
         *
         * @param ern The Euclid resource name (ERN) of the object to search for.
         * @return The matching object, or an empty optional if no match is found.
         */
        [[nodiscard]]
        virtual std::optional<Entity::ESM::Object> findObjectByErn(const std::string &ern) const = 0;

        /**
         * @brief Retrieves the total number of objects in the repository.
         *
         * @return The total number of objects as a long integer.
         */
        [[nodiscard]]
        virtual long countObjects() const = 0;

        /**
         * @brief Retrieves the total number og objects in a bucket, optionally filtered by object key prefix.
         *
         * @paranm bucketErn bucket ERN
         * @paranm prefix object key prefix
         * @return The total number of messages as a long integer.
         */
        [[nodiscard]]
        virtual long countObjects(const std::string &bucketErn, const std::string &prefix) const = 0;

        /**
         * @brief Finds and retrieves all objects of a bucket, optionally filtered, paged and sorted.
         *
         * @param bucketErn The ERN of the bucket the object belongs to.
         * @param prefix only buckets whose name starts with this prefix are returned; empty matches all buckets
         * @param pageSize maximum number of buckets to return; 0 or less means no limit
         * @param pageIndex zero-based page index, applied when pageSize is set
         * @param sortColumn field to sort by (e.g. "name", "ern"); empty means unsorted
         * @return matching, paged and sorted list of buckets
         */
        [[nodiscard]]
        virtual std::vector<Entity::ESM::Object> listObjects(const std::string &bucketErn, const std::string &prefix, long pageSize, long pageIndex, const std::string &sortColumn) const = 0;

        /**
         * @brief Removes a object by its ERN.
         *
         * @param ern The Euclid resource name (ERN) of the object to be removed.
         */
        virtual void deleteObjectByErn(const std::string &ern) = 0;
    };

}// namespace Euclid::Database