// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

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
#include <euclid/database/entity/eam/Grant.h>
#include <euclid/database/entity/eam/Role.h>
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
         * @brief Searches for a user by the identity provider subject it signs in as.
         *
         * @par
         * How a federated login finds its user (see Entity::EAM::User::federatedSubject). An empty
         * provider or subject matches nothing rather than matching every password user, which is
         * what a lookup by field value would otherwise do.
         *
         * @param provider Which federation the subject belongs to: "oidc" or "saml".
         * @param subject The provider's subject - an OIDC "sub" claim or a SAML NameID.
         * @return The matching user, or an empty optional if no match is found.
         */
        [[nodiscard]]
        virtual std::optional<Entity::EAM::User> findUserByFederatedSubject(const std::string &provider, const std::string &subject) const = 0;

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
        virtual std::vector<Entity::EAM::User> listUsers(const std::string &prefix, long pageSize, long pageIndex, const std::string &sortColumn, const std::string &sortDirection = "asc") const = 0;

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
        virtual std::vector<Entity::EAM::UserGroup> listUserGroups(const std::string &prefix, long pageSize, long pageIndex, const std::string &sortColumn, const std::string &sortDirection = "asc") const = 0;

        /**
         * @brief Removes a user group by its name.
         *
         * @param name The name of the group to be removed.
         */
        virtual void deleteUserGroup(const std::string &name) const = 0;

        // ── Roles and grants ────────────────────────────────────────────────
        //
        // A role is a named set of permissions belonging to one account; a grant gives one role to
        // one principal, scoped by account, namespace and resource. The built-in roles
        // (Core::BuiltinRoles) are never stored and never returned by any of these - they are
        // computed, and a caller that wants "every role this account can bind" asks for both.
        //
        // See docs/role-concept.md.

        /**
         * @brief Inserts a new role or updates an existing one.
         *
         * @param role the role to insert or update; matched on its account and name.
         * @return the stored role.
         */
        virtual Entity::EAM::Role upsertRole(Entity::EAM::Role &role) = 0;

        /**
         * @brief Searches for a role by name, within one account.
         *
         * @param accountId the owning account - a role name is unique only within one.
         * @param name the role name.
         * @return the matching role, or an empty optional.
         */
        [[nodiscard]]
        virtual std::optional<Entity::EAM::Role> findRoleByName(const std::string &accountId, const std::string &name) const = 0;

        /**
         * @brief The number of stored roles in one account.
         *
         * @param accountId the owning account.
         */
        [[nodiscard]]
        virtual long countRoles(const std::string &accountId) const = 0;

        /**
         * @brief Lists one account's stored roles.
         *
         * @param accountId the owning account.
         * @param prefix only roles whose name starts with this; empty matches all.
         * @param pageSize maximum to return; 0 or less means no limit.
         * @param pageIndex zero-based page index, applied when pageSize is set.
         * @param sortColumn field to sort by, e.g. "name"; empty means unsorted.
         * @param sortDirection "asc" or "desc".
         * @return the matching, paged and sorted roles.
         */
        [[nodiscard]]
        virtual std::vector<Entity::EAM::Role> listRoles(const std::string &accountId, const std::string &prefix, long pageSize,
                                                         long pageIndex, const std::string &sortColumn,
                                                         const std::string &sortDirection = "asc") const = 0;

        /**
         * @brief Removes a role.
         *
         * @par
         * Says nothing about the grants that name it - a caller refuses the deletion while any
         * exist rather than leaving grants pointing at nothing. See findGrantsByRole().
         *
         * @param accountId the owning account.
         * @param name the role name.
         */
        virtual void deleteRole(const std::string &accountId, const std::string &name) const = 0;

        /**
         * @brief Records a grant.
         *
         * @par
         * Inserts rather than upserts: the same role may be granted to the same principal twice
         * with different namespaces or resources, and both are real. Revoking takes the grant's
         * own id.
         *
         * @param grant the grant to record.
         * @return the stored grant, with its id.
         */
        virtual Entity::EAM::Grant addGrant(Entity::EAM::Grant &grant) = 0;

        /**
         * @brief Every grant held by any of these principals.
         *
         * @par
         * The one query authorization runs. Takes the whole list - the user's own ERN and every
         * group they belong to - because a caller's rights are the union of all of them, and one
         * query is what makes that affordable per request.
         *
         * @param principals user and user-group ERNs.
         * @return their grants, in no particular order.
         */
        [[nodiscard]]
        virtual std::vector<Entity::EAM::Grant> findGrantsByPrincipals(const std::vector<std::string> &principals) const = 0;

        /**
         * @brief Every grant of one role, for answering "who can do this" and for refusing to
         * delete a role that is still in use.
         *
         * @param accountId the role's account.
         * @param role the role name.
         */
        [[nodiscard]]
        virtual std::vector<Entity::EAM::Grant> findGrantsByRole(const std::string &accountId, const std::string &role) const = 0;

        /**
         * @brief Removes one grant by its id.
         *
         * @param oid the grant's id, as addGrant() returned it.
         */
        virtual void deleteGrant(const std::string &oid) const = 0;

        /**
         * @brief Removes every grant held by a principal, for when a user or group is deleted.
         *
         * @par
         * Without this, deleting a user and creating another with the same ID would hand the new
         * one the old one's rights.
         *
         * @param principal user or user-group ERN.
         */
        virtual void deleteGrantsByPrincipal(const std::string &principal) const = 0;

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
        virtual std::vector<Entity::EAM::Account> listAccounts(const std::string &prefix, long pageSize, long pageIndex, const std::string &sortColumn, const std::string &sortDirection = "asc") const = 0;

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
        virtual std::vector<Entity::EAM::Namespace> listNamespaces(const std::string &accountId, const std::string &prefix, long pageSize, long pageIndex, const std::string &sortColumn, const std::string &sortDirection = "asc") const = 0;

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