//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <algorithm>
#include <optional>
#include <string>
#include <vector>

// Euclid includes
#include <euclid/database/entity/eam/Account.h>
#include <euclid/database/entity/eam/Namespace.h>
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
         * @brief Searches for a user by its ERN.
         *
         * @param ern The ERN to search for.
         * @return The matching user, or an empty optional if no match is found.
         */
        [[nodiscard]]
        virtual std::optional<Entity::EAM::User> findUserByErn(const std::string &ern) const = 0;

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
         * @brief Checks if a user with the specified ERN exists in the repository.
         *
         * @param ern The user ERN to check for existence.
         * @return True if a user with the given ERN exists, otherwise false.
         */
        [[nodiscard]]
        virtual bool userErnExists(const std::string &ern) const = 0;

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
         * @brief Checks if a group with the specified ERN exists in the repository.
         *
         * @param ern The group ERN to check for existence.
         * @return True if a group with the given ERN exists, otherwise false.
         */
        [[nodiscard]]
        virtual bool userGroupErnExists(const std::string &ern) const = 0;

        /**
         * @brief Searches for a user group by its name.
         *
         * @param name The user group name to search for.
         * @return The matching user group, or an empty optional if no match is found.
         */
        [[nodiscard]]
        virtual std::optional<Entity::EAM::UserGroup> findUserGroupByName(const std::string &name) const = 0;

        /**
         * @brief Searches for a user group by its ERN.
         *
         * @param ern The user group ERN to search for.
         * @return The matching user group, or an empty optional if no match is found.
         */
        [[nodiscard]]
        virtual std::optional<Entity::EAM::UserGroup> findUserGroupByErn(const std::string &ern) const = 0;

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

        /**
         * @brief Inserts a new account or updates an existing one in the repository.
         *
         * @param account The account to be inserted or updated in the repository.
         */
        virtual Entity::EAM::Account upsertAccount(Entity::EAM::Account &account) = 0;

        /**
         * @brief Checks if an account with the specified account ID exists in the repository.
         *
         * @param accountId The account ID to check for existence.
         * @return True if an account with the given account ID exists, otherwise false.
         */
        [[nodiscard]]
        virtual bool accountExists(const std::string &accountId) const = 0;

        /**
         * @brief Checks if an account with the specified ERN exists in the repository.
         *
         * @param ern The account ERN to check for existence.
         * @return True if an account with the given ERN exists, otherwise false.
         */
        [[nodiscard]]
        virtual bool accountErnExists(const std::string &ern) const = 0;

        /**
         * @brief Searches for an account by its account ID.
         *
         * @param accountId The account ID to search for.
         * @return The matching account, or an empty optional if no match is found.
         */
        [[nodiscard]]
        virtual std::optional<Entity::EAM::Account> findAccountByAccountId(const std::string &accountId) const = 0;

        /**
         * @brief Searches for an account by its ERN.
         *
         * @param ern The ERN to search for.
         * @return The matching account, or an empty optional if no match is found.
         */
        [[nodiscard]]
        virtual std::optional<Entity::EAM::Account> findAccountByErn(const std::string &ern) const = 0;

        /**
         * @brief Retrieves the total count of accounts in the repository.
         *
         * @return The total number of accounts.
         */
        [[nodiscard]]
        virtual long countAccounts() const = 0;

        /**
         * @brief Retrieves a list of accounts
         *
         * Only accounts with the given prefix are returned. The response is also paged and sorted when the parameter are given.
         *
         * @param prefix only accounts whose accountId starts with this prefix are returned; empty matches all accounts
         * @param pageSize maximum number of accounts to return; 0 or less means no limit
         * @param pageIndex zero-based page index, applied when pageSize is set
         * @param sortColumn field to sort by (e.g. "accountId", "name"); empty means unsorted
         * @return matching, paged and sorted list of accounts
         */
        [[nodiscard]]
        virtual std::vector<Entity::EAM::Account> listAccounts(const std::string &prefix, long pageSize, long pageIndex, const std::string &sortColumn) const = 0;

        /**
         * @brief Removes an account by its account ID.
         *
         * @param accountId The account ID of the account to be removed.
         */
        virtual void deleteAccount(const std::string &accountId) const = 0;

        /**
         * @brief Inserts a new namespace or updates an existing one in the repository.
         *
         * @param ns The namespace to be inserted or updated in the repository.
         */
        virtual Entity::EAM::Namespace upsertNamespace(Entity::EAM::Namespace &ns) = 0;

        /**
         * @brief Checks if a namespace with the specified name exists under an account.
         *
         * @param accountId The account the namespace should belong to.
         * @param name The namespace name to check for existence.
         * @return True if a matching namespace exists, otherwise false.
         */
        [[nodiscard]]
        virtual bool namespaceExists(const std::string &accountId, const std::string &name) const = 0;

        /**
         * @brief Checks if a namespace with the specified ERN exists in the repository.
         *
         * @param ern The namespace ERN to check for existence.
         * @return True if a namespace with the given ERN exists, otherwise false.
         */
        [[nodiscard]]
        virtual bool namespaceErnExists(const std::string &ern) const = 0;

        /**
         * @brief Searches for a namespace by its account ID and name.
         *
         * @param accountId The account the namespace belongs to.
         * @param name The namespace name to search for.
         * @return The matching namespace, or an empty optional if no match is found.
         */
        [[nodiscard]]
        virtual std::optional<Entity::EAM::Namespace> findNamespaceByName(const std::string &accountId, const std::string &name) const = 0;

        /**
         * @brief Searches for a namespace by its ERN.
         *
         * @param ern The ERN to search for.
         * @return The matching namespace, or an empty optional if no match is found.
         */
        [[nodiscard]]
        virtual std::optional<Entity::EAM::Namespace> findNamespaceByErn(const std::string &ern) const = 0;

        /**
         * @brief Retrieves the total count of namespaces under an account.
         *
         * @param accountId The account to count namespaces for.
         * @return The total number of namespaces under accountId.
         */
        [[nodiscard]]
        virtual long countNamespaces(const std::string &accountId) const = 0;

        /**
         * @brief Retrieves a list of namespaces under an account.
         *
         * Only namespaces with the given prefix are returned. The response is also paged and sorted when the parameter are given.
         *
         * @param accountId only namespaces belonging to this account are returned
         * @param prefix only namespaces whose name starts with this prefix are returned; empty matches all namespaces
         * @param pageSize maximum number of namespaces to return; 0 or less means no limit
         * @param pageIndex zero-based page index, applied when pageSize is set
         * @param sortColumn field to sort by (e.g. "name"); empty means unsorted
         * @return matching, paged and sorted list of namespaces
         */
        [[nodiscard]]
        virtual std::vector<Entity::EAM::Namespace> listNamespaces(const std::string &accountId, const std::string &prefix, long pageSize, long pageIndex, const std::string &sortColumn) const = 0;

        /**
         * @brief Removes a namespace by its account ID and name.
         *
         * @param accountId The account the namespace belongs to.
         * @param name The name of the namespace to be removed.
         */
        virtual void deleteNamespace(const std::string &accountId, const std::string &name) const = 0;

    };

    /**
     * @brief Name of the EAM user group whose membership determines administrator privileges.
     *
     * Administrator status has no dedicated field on User - it's entirely derived from
     * membership in this group (see IsEamAdmin()), granted/revoked the same way any other group
     * membership is, via the user-group-add-user/user-group-remove-user actions.
     */
    constexpr auto kEamAdministratorGroupName = "administrator";

    /**
     * @brief Whether userId is an EAM administrator, i.e. a member of the group named
     * kEamAdministratorGroupName.
     *
     * @param repo repository to look up the administrator group in
     * @param userId user ID to check
     * @return true if userId is a member of the administrator group
     */
    inline bool IsEamAdmin(const IEamRepository &repo, const std::string &userId) {
        const auto group = repo.findUserGroupByName(kEamAdministratorGroupName);
        return group.has_value() && std::ranges::contains(group->userIds, userId);
    }

}// namespace Euclid::Database