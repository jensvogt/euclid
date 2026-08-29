//
// Created by vogje01 on 5/24/26.
//

#pragma once

// C++ includes
#include <map>
#include <optional>
#include <string>
#include <vector>

// Euclid includes
#include <euclid/database/entity/ekm/Key.h>

namespace Euclid::Database {

    /**
     * @brief Interface for EKM repository operations.
     *
     * Provides an abstraction for storing, retrieving, and managing
     * EKM-related data.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class IEkmRepository {

    public:

        /**
         * @brief Virtual destructor for the IEkmRepository interface.
         *
         * Ensures derived classes' destructor is invoked correctly
         * during object destruction to release resources.
         */
        virtual ~IEkmRepository() = default;

        /**
         * @brief Inserts a new key or updates an existing one in the repository.
         *
         * If a module with the same identifier already exists, its data will be updated
         * with the provided queue information. Otherwise, a new key will be added
         * to the repository.
         *
         * @param key The key to be inserted or updated in the repository.
         */
        virtual Entity::EKM::Key upsertKey(Entity::EKM::Key &key) = 0;

        /**
         * @brief Finds and retrieves all available entities or objects.
         *
         * @param accountId only queues belonging to this account are returned
         * @param namespaceName only queues in this namespace are returned; empty means don't filter by namespace
         * @param prefix only queues whose name starts with this prefix are returned; empty matches all queues
         * @param pageSize maximum number of queues to return; 0 or less means no limit
         * @param pageIndex zero-based page index, applied when pageSize is set
         * @param sortColumn field to sort by (e.g. "name", "arn"); empty means unsorted
         * @param sortDirection "asc" or "desc"; anything else is treated as "desc"
         * @return A collection containing all entities or objects found.
         */
        [[nodiscard]]
        virtual std::vector<Entity::EKM::Key> listKeys(const std::string &accountId, const std::string &namespaceName, const std::string &prefix, long pageSize, long pageIndex, const std::string &sortColumn, const std::string &sortDirection = "asc") const = 0;

        /**
         * @brief Retrieves the total count of keys in the repository.
         *
         * @param accountId only keys belonging to this account are counted
         * @param namespaceName only keys in this namespace are counted; empty means don't filter by namespace
         * @param prefix only keys whose name starts with this prefix are counted; empty means don't filter by name
         * @return The total number of keys as a long integer.
         */
        [[nodiscard]]
        virtual long countKeys(const std::string &accountId, const std::string &namespaceName, const std::string &prefix = "") const = 0;

    };

}// namespace Euclid::Database