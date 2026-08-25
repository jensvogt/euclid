//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <optional>
#include <string>
#include <vector>

// Euclid includes
#include <euclid/database/entity/eam/User.h>
#include <euclid/database/entity/eam/UserGroup.h>

namespace Euclid::Database {

    /**
     * @brief Interface for access (user/login) repository operations.
     *
     * Provides an abstraction for storing and looking up users.
     */
    class IEamRepository {

    public:

        virtual ~IEamRepository() = default;

        /**
         * @brief Inserts a new user or updates an existing one in the repository.
         *
         * @param user The user to be inserted or updated in the repository.
         */
        virtual Entity::EAM::User upsertUser(Entity::EAM::User &user) = 0;

        /**
         * @brief Removes a user by its user ID.
         *
         * @param userId The user ID of the user to be removed.
         */
        virtual void deleteUser(const std::string &userId) const = 0;

        /**
         * @brief Searches for a user by its user ID.
         *
         * @param userId The user ID to search for.
         * @return The matching user, or an empty optional if no match is found.
         */
        [[nodiscard]]
        virtual std::optional<Entity::EAM::User> findUserByUserId(const std::string &userId) const = 0;

        /**
         * @brief Searches for a user by its email address.
         *
         * @param email The email address to search for.
         * @return The matching user, or an empty optional if no match is found.
         */
        [[nodiscard]]
        virtual std::optional<Entity::EAM::User> findUserByEmail(const std::string &email) const = 0;

        /**
         * @brief Searches for the user owning a given access key ID.
         *
         * Used by SigV4 signature verification to look up the secret to check a signature
         * against - the caller identifies itself by access key ID, not user ID.
         *
         * @param accessKeyId The access key ID to search for.
         * @return The matching user, or an empty optional if no key with this ID exists.
         */
        [[nodiscard]]
        virtual std::optional<Entity::EAM::User> findUserByAccessKeyId(const std::string &accessKeyId) const = 0;

        /**
         * @brief Checks if a user with the specified user ID exists in the repository.
         *
         * @param userId The user ID to check for existence.
         * @return True if a user with the given user ID exists, otherwise false.
         */
        [[nodiscard]]
        virtual bool userExists(const std::string &userId) const = 0;

        /**
         * @brief Retrieves the total count of users in the repository.
         *
         * Used to detect an empty user store, e.g. to bootstrap the first
         * administrator account.
         *
         * @return The total number of users.
         */
        [[nodiscard]]
        virtual long countUsers() const = 0;

        /**
         * @brief Retrieves a list of users
         *
         * Only users with the given prefix are returned. The response is also paged and sorted when the parameter are given.
         *
         * @param prefix only users whose userId starts with this prefix are returned; empty matches all users
         * @param pageSize maximum number of users to return; 0 or less means no limit
         * @param pageIndex zero-based page index, applied when pageSize is set
         * @param sortColumn field to sort by (e.g. "userId", "email"); empty means unsorted
         * @return matching, paged and sorted list of users
         */
        [[nodiscard]]
        virtual std::vector<Entity::EAM::User> listUsers(const std::string &prefix, long pageSize, long pageIndex, const std::string &sortColumn) const = 0;

        /**
         * @brief Inserts a new user group or updates an existing one in the repository.
         *
         * @param group The group to be inserted or updated in the repository.
         */
        virtual Entity::EAM::UserGroup upsertUserGroup(Entity::EAM::UserGroup &group) = 0;

        /**
         * @brief Checks if a group with the specified name exists in the repository.
         *
         * @param name The group name to check for existence.
         * @return True if a group with the given name exists, otherwise false.
         */
        [[nodiscard]]
        virtual bool userGroupExists(const std::string &name) const = 0;

        /**
         * @brief Retrieves the total count of user groups in the repository.
         *
         * @return The total number of user groups.
         */
        [[nodiscard]]
        virtual long countUserGroups() const = 0;

        /**
         * @brief Retrieves a list of user groups
         *
         * Only user groups with the given prefix are returned. The response is also paged and sorted when the parameter are given.
         *
         * @param prefix only user groups whose name starts with this prefix are returned; empty matches all user groups
         * @param pageSize maximum number of user groups to return; 0 or less means no limit
         * @param pageIndex zero-based page index, applied when pageSize is set
         * @param sortColumn field to sort by (e.g. "name", "description"); empty means unsorted
         * @return matching, paged and sorted list of user groups
         */
        [[nodiscard]]
        virtual std::vector<Entity::EAM::UserGroup> listUserGroups(const std::string &prefix, long pageSize, long pageIndex, const std::string &sortColumn) const = 0;

        /**
         * @brief Removes a user group by its name.
         *
         * @param name The name of the group to be removed.
         */
        virtual void deleteUserGroup(const std::string &name) const = 0;

    };

}// namespace Euclid::Database