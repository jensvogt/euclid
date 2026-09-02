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
#include <euclid/database/entity/esm/Subscription.h>

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
         * @param sortDirection "asc" or "desc"; anything else is treated as "desc"
         * @return matching, paged and sorted list of buckets
         */
        [[nodiscard]]
        virtual std::vector<Entity::ESM::Bucket> listBuckets(const std::string &accountId, const std::string &namespaceName, const std::string &prefix, long pageSize, long pageIndex, const std::string &sortColumn, const std::string &sortDirection = "asc") const = 0;

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
         * @param prefix only buckets whose name starts with this prefix are counted; empty means don't filter by name
         * @return The total number of buckets as a long integer.
         */
        [[nodiscard]]
        virtual long countBuckets(const std::string &accountId, const std::string &namespaceName, const std::string &prefix = "") const = 0;

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
         * @param includeDirectories whether directory keys (see Entity::ESM::IsDirectoryKey())
         * are counted; they are left out by default, so a bucket counts the files it holds
         * @return The total number of messages as a long integer.
         */
        [[nodiscard]]
        virtual long countObjects(const std::string &bucketErn, const std::string &prefix, bool includeDirectories = false) const = 0;

        /**
         * @brief Retrieves the number of objects in a bucket whose bytes are stored encrypted.
         *
         * @par
         * An object is encrypted if it names the key it was written under, so this counts the
         * objects that still need that key to be readable - which is what makes it worth asking:
         * turning encryption off for a bucket does not decrypt anything, and this is the number
         * that says whether the key can be retired yet.
         *
         * @param bucketErn bucket ERN
         * @return the number of encrypted objects.
         */
        [[nodiscard]]
        virtual long countEncryptedObjects(const std::string &bucketErn) const = 0;

        /**
         * @brief Finds and retrieves all objects of a bucket, optionally filtered, paged and sorted.
         *
         * @param bucketErn The ERN of the bucket the object belongs to.
         * @param prefix only buckets whose name starts with this prefix are returned; empty matches all buckets
         * @param pageSize maximum number of buckets to return; 0 or less means no limit
         * @param pageIndex zero-based page index, applied when pageSize is set
         * @param sortColumn field to sort by (e.g. "name", "ern"); empty means unsorted
         * @param sortDirection "asc" or "desc"; anything else is treated as "desc"
         * @param includeDirectories whether directory keys (see Entity::ESM::IsDirectoryKey())
         * are returned; they are left out by default, so a listing shows the files a bucket holds
         * @return matching, paged and sorted list of buckets
         */
        [[nodiscard]]
        virtual std::vector<Entity::ESM::Object> listObjects(const std::string &bucketErn, const std::string &prefix, long pageSize, long pageIndex, const std::string &sortColumn, const std::string &sortDirection = "asc", bool includeDirectories = false) const = 0;

        /**
         * @brief Removes a object by its ERN.
         *
         * @param ern The Euclid resource name (ERN) of the object to be removed.
         */
        virtual void deleteObjectByErn(const std::string &ern) = 0;

        /**
         * @brief Repoints every object of a renamed bucket at its new name.
         *
         * @par
         * A bucket's name is in its ERN, and an object's ERN carries the bucket's name as well
         * ("...:object:<bucket>/<key>"), so renaming a bucket is not a change to one document but
         * to every object in it. Done here rather than by reading each object and writing it back
         * because it is one edit repeated, and a bucket can hold a lot of objects.
         *
         * @param oldBucketErn the bucket's ERN before the rename
         * @param newBucketErn the bucket's ERN after it
         * @param oldName the bucket's name before the rename
         * @param newName the bucket's name after it
         * @return number of objects rewritten
         */
        /**
         * @brief Gives an existing bucket another name and ERN, in place.
         *
         * @par
         * Not upsertBucket(): that one identifies a bucket by its account, namespace and name, so
         * handing it a changed name inserts a second bucket rather than renaming the first. A
         * rename has to address the document that already exists, which is what this does.
         *
         * @param ern the bucket's current ERN
         * @param newName the name it should have
         * @param newErn the ERN that name gives it
         * @return the bucket as it now is, or nothing if no bucket has that ERN
         */
        virtual std::optional<Entity::ESM::Bucket> renameBucket(const std::string &ern, const std::string &newName, const std::string &newErn) = 0;

        /**
         * @brief Points every subscription of a bucket at its new ERN.
         *
         * @par
         * Also not an upsert, and for the same reason: a subscription is identified by what it
         * watches, which is exactly what a rename changes.
         *
         * @param oldSourceErn the bucket's ERN before the rename
         * @param newSourceErn the bucket's ERN after it
         * @return number of subscriptions repointed
         */
        virtual long repointSubscriptions(const std::string &oldSourceErn, const std::string &newSourceErn) = 0;

        virtual long renameBucketObjects(const std::string &oldBucketErn, const std::string &newBucketErn,
                                          const std::string &oldName, const std::string &newName) = 0;

        /**
         * @brief Creates (or, for a matching existing sourceErn/type/targetErn, refreshes) a
         * subscription that fans out object-created notifications from sourceErn to targetErn.
         *
         * @param subscription the subscription to be inserted or updated in the repository.
         */
        virtual Entity::ESM::Subscription upsertSubscription(Entity::ESM::Subscription &subscription) = 0;

        /**
         * @brief Lists every subscription fanning out from a bucket.
         *
         * @param sourceErn ERN of the bucket whose subscriptions are listed.
         * @return the bucket's subscriptions; empty if it has none.
         */
        [[nodiscard]]
        virtual std::vector<Entity::ESM::Subscription> listSubscriptionsBySourceErn(const std::string &sourceErn) const = 0;

        /**
         * @brief Deletes a subscription by its ERN. Deleting an ERN with no matching subscription
         * is not an error.
         *
         * @param ern subscription ERN
         */
        virtual void deleteSubscriptionByErn(const std::string &ern) = 0;
    };

}// namespace Euclid::Database