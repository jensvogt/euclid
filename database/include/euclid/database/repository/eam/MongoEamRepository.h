//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ standard includes
#include <string>
#include <vector>

// MongoDB includes
#include <mongocxx/options/find.hpp>

// Euclid includes
#include <euclid/core/LogStream.h>
#include <euclid/database/Database.h>
#include <euclid/database/entity/eam/Account.h>
#include <euclid/database/entity/eam/Namespace.h>
#include <euclid/database/entity/eam/User.h>
#include <euclid/database/entity/eam/UserGroup.h>
#include <euclid/database/repository/eam/IEamRepository.h>

namespace Euclid::Database {

    using namespace bsoncxx::builder::basic;

    /**
     * @brief Access MongoDB database.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class MongoEamRepository final : public IEamRepository {

    public:

        /**
         * @brief Constructor
         */
        explicit MongoEamRepository();

        /**
         * @brief Singleton instance
         */
        static MongoEamRepository &instance() {
            static MongoEamRepository accessDatabase;
            return accessDatabase;
        }

        /**
         * @brief Update an existing user or insert a new user.
         *
         * @param user user to upsert
         * @return upserted user.
         */
        Entity::EAM::User upsertUser(Entity::EAM::User &user) override;

        /**
         * @brief Returns a user by user ID.
         *
         * @param userId user ID to find.
         * @return optional of found user.
         */
        [[nodiscard]]
        std::optional<Entity::EAM::User> findUserByUserId(const std::string &userId) const override;

        /**
         * @brief Returns a user by email.
         *
         * @param email user ID to find
         * @return optional of found user
         */
        [[nodiscard]]
        std::optional<Entity::EAM::User> findUserByEmail(const std::string &email) const override;

        /**
         * @brief Returns a user by ERN.
         *
         * @param ern ERN to find
         * @return optional of found user
         */
        [[nodiscard]]
        std::optional<Entity::EAM::User> findUserByErn(const std::string &ern) const override;

        /**
         * @brief Returns a user by accessKeyId.
         *
         * @param accessKeyId user accessKeyId to find
         * @return optional of found user.
         */
        [[nodiscard]]
        std::optional<Entity::EAM::User> findUserByAccessKeyId(const std::string &accessKeyId) const override;

        /**
         * @brief Checks whether a user exists.
         *
         * @param userId user ID to find
         * @return true if existing, otherwise false.
         */
        [[nodiscard]]
        bool userExists(const std::string &userId) const override;

        /**
         * @brief Checks if a user with the specified ERN exists in the repository.
         *
         * @param ern The user ERN to check for existence.
         * @return True if a user with the given ERN exists, otherwise false.
         */
        [[nodiscard]]
        bool userErnExists(const std::string &ern) const override;

        /**
         * @brief Returns the number of existing users.
         *
         * @return number of existing users.
         */
        [[nodiscard]]
        long countUsers() const override;

        /**
         * @brief Returns a user group by name.
         *
         * @param name name to find
         * @return optional of found user group
         */
        [[nodiscard]]
        std::optional<Entity::EAM::UserGroup> findUserGroupByName(const std::string &name) const override;

        /**
         * @brief Returns a user group by ERN.
         *
         * @param ern ERN to find
         * @return optional of found user group
         */
        [[nodiscard]]
        std::optional<Entity::EAM::UserGroup> findUserGroupByErn(const std::string &ern) const override;

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
        std::vector<Entity::EAM::User> listUsers(const std::string &prefix, long pageSize, long pageIndex, const std::string &sortColumn, const std::string &sortDirection = "asc") const override;

        /**
         * @brief Deletes a user by user ID.
         *
         * @param userId user ID of the user to delete.
         */
        void deleteUser(const std::string &userId) const override;

        /**
         * @brief Update an existing user group or insert a new one.
         *
         * @param group group to upsert
         * @return upserted group.
         */
        Entity::EAM::UserGroup upsertUserGroup(Entity::EAM::UserGroup &group) override;

        /**
         * @brief Checks whether a user group exists.
         *
         * @param name group name to find
         * @return true if existing, otherwise false.
         */
        [[nodiscard]]
        bool userGroupExists(const std::string &name) const override;

        /**
         * @brief Checks if a group with the specified ERN exists in the repository.
         *
         * @param ern The group ERN to check for existence.
         * @return True if a group with the given ERN exists, otherwise false.
         */
        [[nodiscard]]
        bool userGroupErnExists(const std::string &ern) const override;

        /**
         * @brief Returns the number of existing user groups.
         *
         * @return number of existing user groups.
         */
        [[nodiscard]]
        long countUserGroups() const override;

        /**
         * @brief Retrieves a list of user groups
         *
         * Only user groups with the given prefix are returned. The response is also paged and sorted when the parameter are given.
         *
         * @param prefix only user groups whose name starts with this prefix are returned; empty matches all user groups
         * @param pageSize maximum number of user groups to return; 0 or less means no limit
         * @param pageIndex zero-based page index, applied when pageSize is set
         * @param sortColumn field to sort by (e.g. "userId", "email"); empty means unsorted
         * @return matching, paged and sorted list of users
         */
        [[nodiscard]]
        std::vector<Entity::EAM::UserGroup> listUserGroups(const std::string &prefix, long pageSize, long pageIndex, const std::string &sortColumn, const std::string &sortDirection = "asc") const override;

        /**
         * @brief Removes a user group by its name.
         *
         * @param name The name of the group to be removed.
         */
        void deleteUserGroup(const std::string &name) const override;

        /**
         * @brief Update an existing account or insert a new account.
         *
         * @param account account to upsert
         * @return upserted account.
         */
        Entity::EAM::Account upsertAccount(Entity::EAM::Account &account) override;

        /**
         * @brief Checks whether an account exists.
         *
         * @param accountId account ID to find.
         * @return true if existing, otherwise false.
         */
        [[nodiscard]]
        bool accountExists(const std::string &accountId) const override;

        /**
         * @brief Checks if an account with the specified ERN exists in the repository.
         *
         * @param ern The account ERN to check for existence.
         * @return True if an account with the given ERN exists, otherwise false.
         */
        [[nodiscard]]
        bool accountErnExists(const std::string &ern) const override;

        /**
         * @brief Returns an account by account ID.
         *
         * @param accountId account ID to find.
         * @return optional of found account.
         */
        [[nodiscard]]
        std::optional<Entity::EAM::Account> findAccountByAccountId(const std::string &accountId) const override;

        /**
         * @brief Returns an account by ERN.
         *
         * @param ern ERN to find
         * @return optional of found account
         */
        [[nodiscard]]
        std::optional<Entity::EAM::Account> findAccountByErn(const std::string &ern) const override;

        /**
         * @brief Returns the number of existing accounts.
         *
         * @return number of existing accounts.
         */
        [[nodiscard]]
        long countAccounts() const override;

        /**
         * @brief Retrieves a list of accounts.
         *
         * @param prefix only accounts whose accountId starts with this prefix are returned; empty matches all accounts
         * @param pageSize maximum number of accounts to return; 0 or less means no limit
         * @param pageIndex zero-based page index, applied when pageSize is set
         * @param sortColumn field to sort by; empty means unsorted
         * @return matching, paged and sorted list of accounts
         */
        [[nodiscard]]
        std::vector<Entity::EAM::Account> listAccounts(const std::string &prefix, long pageSize, long pageIndex, const std::string &sortColumn, const std::string &sortDirection = "asc") const override;

        /**
         * @brief Deletes an account by account ID.
         *
         * @param accountId account ID of the account to delete.
         */
        void deleteAccount(const std::string &accountId) const override;

        /**
         * @brief Update an existing namespace or insert a new one.
         *
         * @param ns namespace to upsert
         * @return upserted namespace.
         */
        Entity::EAM::Namespace upsertNamespace(Entity::EAM::Namespace &ns) override;

        /**
         * @brief Checks whether a namespace exists under an account.
         *
         * @param accountId account the namespace should belong to.
         * @param name namespace name to find.
         * @return true if existing, otherwise false.
         */
        [[nodiscard]]
        bool namespaceExists(const std::string &accountId, const std::string &name) const override;

        /**
         * @brief Checks if a namespace with the specified ERN exists in the repository.
         *
         * @param ern The namespace ERN to check for existence.
         * @return True if a namespace with the given ERN exists, otherwise false.
         */
        [[nodiscard]]
        bool namespaceErnExists(const std::string &ern) const override;

        /**
         * @brief Returns a namespace by account ID and name.
         *
         * @param accountId account the namespace belongs to.
         * @param name namespace name to find.
         * @return optional of found namespace.
         */
        [[nodiscard]]
        std::optional<Entity::EAM::Namespace> findNamespaceByName(const std::string &accountId, const std::string &name) const override;

        /**
         * @brief Returns a namespace by ERN.
         *
         * @param ern ERN to find
         * @return optional of found namespace
         */
        [[nodiscard]]
        std::optional<Entity::EAM::Namespace> findNamespaceByErn(const std::string &ern) const override;

        /**
         * @brief Returns the number of existing namespaces under an account.
         *
         * @param accountId account to count namespaces for.
         * @return number of existing namespaces.
         */
        [[nodiscard]]
        long countNamespaces(const std::string &accountId) const override;

        /**
         * @brief Retrieves a list of namespaces under an account.
         *
         * @param accountId only namespaces belonging to this account are returned
         * @param prefix only namespaces whose name starts with this prefix are returned; empty matches all namespaces
         * @param pageSize maximum number of namespaces to return; 0 or less means no limit
         * @param pageIndex zero-based page index, applied when pageSize is set
         * @param sortColumn field to sort by; empty means unsorted
         * @return matching, paged and sorted list of namespaces
         */
        [[nodiscard]]
        std::vector<Entity::EAM::Namespace> listNamespaces(const std::string &accountId, const std::string &prefix, long pageSize, long pageIndex, const std::string &sortColumn, const std::string &sortDirection = "asc") const override;

        /**
         * @brief Removes a namespace by account ID and name.
         *
         * @param accountId account the namespace belongs to.
         * @param name name of the namespace to be removed.
         */
        void deleteNamespace(const std::string &accountId, const std::string &name) const override;

    private:

        static constexpr auto USER_COLLECTION = "eam_user";
        static constexpr auto USER_GROUP_COLLECTION = "eam_usergroup";
        static constexpr auto ACCOUNT_COLLECTION = "eam_account";
        static constexpr auto NAMESPACE_COLLECTION = "eam_namespace";

        /**
         * @brief Creates the indexes required for efficient user lookup, if they do not already exist.
         */
        static void ensureIndexes();
    };

}// namespace Euclid::Database