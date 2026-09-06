//
// Created by vogje01 on 8/16/26.
//
#pragma once

// C++ includes
#include <algorithm>
#include <mutex>
#include <ranges>
#include <unordered_map>

// Euclid includes
#include <euclid/core/UuidUtils.h>
#include <euclid/database/entity/eam/Account.h>
#include <euclid/database/entity/eam/Namespace.h>
#include <euclid/database/entity/eam/User.h>
#include <euclid/database/entity/eam/UserGroup.h>
#include <euclid/database/repository/eam/IEamRepository.h>

namespace Euclid::Database {

    /**
     * @brief Access in-memory database.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class MemoryEamRepository final : public IEamRepository {

    public:

        /**
         * @brief Singleton instance
         */
        static MemoryEamRepository &instance() {
            static MemoryEamRepository accessDatabase;
            return accessDatabase;
        }

        Entity::EAM::User upsertUser(Entity::EAM::User &user) override {
            std::lock_guard lock(_mutex);
            if (user.oid.empty()) user.oid = Core::UuidUtils::CreateRandomUuid();
            _userStore[user.userId] = user;
            return user;
        }

        void deleteUser(const std::string &userId) const override {
            std::lock_guard lock(_mutex);
            _userStore.erase(userId);
        }

        std::optional<Entity::EAM::User> findUserByUserId(const std::string &userId) const override {
            std::lock_guard lock(_mutex);
            const auto it = _userStore.find(userId);
            if (it == _userStore.end()) return std::nullopt;
            return it->second;
        }

        std::optional<Entity::EAM::User> findUserByEmail(const std::string &email) const override {
            std::lock_guard lock(_mutex);
            for (const auto &user: _userStore | std::views::values) {
                if (user.email == email) return user;
            }
            return std::nullopt;
        }

        std::optional<Entity::EAM::User> findUserByErn(const std::string &ern) const override {
            std::lock_guard lock(_mutex);
            for (const auto &user: _userStore | std::views::values) {
                if (user.ern == ern) return user;
            }
            return std::nullopt;
        }

        std::optional<Entity::EAM::User> findUserByAccessKeyId(const std::string &accessKeyId) const override {
            std::lock_guard lock(_mutex);
            for (const auto &user: _userStore | std::views::values) {
                for (const auto &key: user.accessKeys) {
                    if (key.accessKeyId == accessKeyId) return user;
                }
            }
            return std::nullopt;
        }

        bool userExists(const std::string &userId) const override {
            std::lock_guard lock(_mutex);
            return _userStore.contains(userId);
        }

        bool userErnExists(const std::string &ern) const override {
            std::lock_guard lock(_mutex);
            return std::ranges::any_of(_userStore | std::views::values, [&ern](const auto &b) {
                return b.ern == ern;
            });
        }

        long countUsers() const override {
            std::lock_guard lock(_mutex);
            return static_cast<long>(_userStore.size());
        }

        std::vector<Entity::EAM::User> listUsers(const std::string &prefix, const long pageSize, const long pageIndex, const std::string &sortColumn, const std::string &sortDirection = "asc") const override {
            std::lock_guard lock(_mutex);

            std::vector<Entity::EAM::User> users;
            for (const auto &user: _userStore | std::views::values) {
                if (prefix.empty() || user.userId.starts_with(prefix)) users.push_back(user);
            }

            std::ranges::sort(users, [&sortColumn, &sortDirection](const Entity::EAM::User &a, const Entity::EAM::User &b) {
                const bool ascending = sortDirection != "desc";
                if (sortColumn == "email") return ascending ? a.email < b.email : b.email < a.email;
                return ascending ? a.userId < b.userId : b.userId < a.userId;
            });

            if (pageSize > 0) {
                const auto offset = std::min<std::size_t>(static_cast<std::size_t>(std::max<long>(pageIndex, 0)) * pageSize, users.size());
                const auto count = std::min<std::size_t>(static_cast<std::size_t>(pageSize), users.size() - offset);
                users = std::vector(users.begin() + static_cast<std::ptrdiff_t>(offset), users.begin() + static_cast<std::ptrdiff_t>(offset + count));
            }

            return users;
        }

        Entity::EAM::UserGroup upsertUserGroup(Entity::EAM::UserGroup &group) override {
            std::lock_guard lock(_mutex);
            if (group.oid.empty()) group.oid = Core::UuidUtils::CreateRandomUuid();
            _groupStore[group.name] = group;
            return group;
        }

        bool userGroupExists(const std::string &name) const override {
            std::lock_guard lock(_mutex);
            return _groupStore.contains(name);
        }

        bool userGroupErnExists(const std::string &ern) const override {
            std::lock_guard lock(_mutex);
            return std::ranges::any_of(_groupStore | std::views::values, [&ern](const auto &b) {
                return b.ern == ern;
            });
        }

        std::optional<Entity::EAM::UserGroup> findUserGroupByName(const std::string &name) const override {
            std::lock_guard lock(_mutex);
            if (const auto it = std::ranges::find_if(_groupStore, [name](const auto &i) { return i.second.name == name; }); it != _groupStore.end()) {
                return it->second;
            }
            return std::nullopt;
        }

        std::optional<Entity::EAM::UserGroup> findUserGroupByErn(const std::string &ern) const override {
            std::lock_guard lock(_mutex);
            if (const auto it = std::ranges::find_if(_groupStore, [ern](const auto &i) { return i.second.ern == ern; }); it != _groupStore.end()) {
                return it->second;
            }
            return std::nullopt;
        }

        long countUserGroups() const override {
            std::lock_guard lock(_mutex);
            return static_cast<long>(_groupStore.size());
        }

        std::vector<Entity::EAM::UserGroup> listUserGroups(const std::string &prefix, const long pageSize, const long pageIndex, const std::string &sortColumn, const std::string &sortDirection = "asc") const override {
            std::lock_guard lock(_mutex);

            std::vector<Entity::EAM::UserGroup> userGroups;
            for (const auto &userGroup: _groupStore | std::views::values) {
                if (prefix.empty() || userGroup.name.starts_with(prefix)) userGroups.push_back(userGroup);
            }

            std::ranges::sort(userGroups, [&sortColumn, &sortDirection](const Entity::EAM::UserGroup &a, const Entity::EAM::UserGroup &b) {
                const bool ascending = sortDirection != "desc";
                if (sortColumn == "description") return ascending ? a.description < b.description : b.description < a.description;
                return ascending ? a.name < b.name : b.name < a.name;
            });

            if (pageSize > 0) {
                const auto offset = std::min<std::size_t>(static_cast<std::size_t>(std::max<long>(pageIndex, 0)) * pageSize, userGroups.size());
                const auto count = std::min<std::size_t>(static_cast<std::size_t>(pageSize), userGroups.size() - offset);
                userGroups = std::vector(userGroups.begin() + static_cast<std::ptrdiff_t>(offset), userGroups.begin() + static_cast<std::ptrdiff_t>(offset + count));
            }

            return userGroups;
        }

        /**
         * @brief Removes a user group by its name.
         *
         * @param name The name of the group to be removed.
         */
        void deleteUserGroup(const std::string &name) const override {
            std::lock_guard lock(_mutex);
            _groupStore.erase(name);
        }

        Entity::EAM::Account upsertAccount(Entity::EAM::Account &account) override {
            std::lock_guard lock(_mutex);
            if (account.oid.empty()) account.oid = Core::UuidUtils::CreateRandomUuid();
            _accountStore[account.accountId] = account;
            return account;
        }

        bool accountExists(const std::string &accountId) const override {
            std::lock_guard lock(_mutex);
            return _accountStore.contains(accountId);
        }

        bool accountErnExists(const std::string &ern) const override {
            std::lock_guard lock(_mutex);
            return std::ranges::any_of(_accountStore | std::views::values, [&ern](const auto &a) {
                return a.ern == ern;
            });
        }

        std::optional<Entity::EAM::Account> findAccountByAccountId(const std::string &accountId) const override {
            std::lock_guard lock(_mutex);
            const auto it = _accountStore.find(accountId);
            if (it == _accountStore.end()) return std::nullopt;
            return it->second;
        }

        std::optional<Entity::EAM::Account> findAccountByErn(const std::string &ern) const override {
            std::lock_guard lock(_mutex);
            for (const auto &account: _accountStore | std::views::values) {
                if (account.ern == ern) return account;
            }
            return std::nullopt;
        }

        long countAccounts() const override {
            std::lock_guard lock(_mutex);
            return static_cast<long>(_accountStore.size());
        }

        std::vector<Entity::EAM::Account> listAccounts(const std::string &prefix, const long pageSize, const long pageIndex, const std::string &sortColumn, const std::string &sortDirection = "asc") const override {
            std::lock_guard lock(_mutex);

            std::vector<Entity::EAM::Account> accounts;
            for (const auto &account: _accountStore | std::views::values) {
                if (prefix.empty() || account.accountId.starts_with(prefix)) accounts.push_back(account);
            }

            std::ranges::sort(accounts, [&sortColumn, &sortDirection](const Entity::EAM::Account &a, const Entity::EAM::Account &b) {
                const bool ascending = sortDirection != "desc";
                if (sortColumn == "name") return ascending ? a.name < b.name : b.name < a.name;
                return ascending ? a.accountId < b.accountId : b.accountId < a.accountId;
            });

            if (pageSize > 0) {
                const auto offset = std::min<std::size_t>(static_cast<std::size_t>(std::max<long>(pageIndex, 0)) * pageSize, accounts.size());
                const auto count = std::min<std::size_t>(static_cast<std::size_t>(pageSize), accounts.size() - offset);
                accounts = std::vector(accounts.begin() + static_cast<std::ptrdiff_t>(offset), accounts.begin() + static_cast<std::ptrdiff_t>(offset + count));
            }

            return accounts;
        }

        void deleteAccount(const std::string &accountId) const override {
            std::lock_guard lock(_mutex);
            _accountStore.erase(accountId);
        }

        Entity::EAM::Namespace upsertNamespace(Entity::EAM::Namespace &ns) override {
            std::lock_guard lock(_mutex);
            if (ns.oid.empty()) ns.oid = Core::UuidUtils::CreateRandomUuid();
            _namespaceStore[namespaceKey(ns.accountId, ns.name)] = ns;
            return ns;
        }

        bool namespaceExists(const std::string &accountId, const std::string &name) const override {
            std::lock_guard lock(_mutex);
            return _namespaceStore.contains(namespaceKey(accountId, name));
        }

        bool namespaceErnExists(const std::string &ern) const override {
            std::lock_guard lock(_mutex);
            return std::ranges::any_of(_namespaceStore | std::views::values, [&ern](const auto &n) {
                return n.ern == ern;
            });
        }

        std::optional<Entity::EAM::Namespace> findNamespaceByName(const std::string &accountId, const std::string &name) const override {
            std::lock_guard lock(_mutex);
            const auto it = _namespaceStore.find(namespaceKey(accountId, name));
            if (it == _namespaceStore.end()) return std::nullopt;
            return it->second;
        }

        std::optional<Entity::EAM::Namespace> findNamespaceByErn(const std::string &ern) const override {
            std::lock_guard lock(_mutex);
            for (const auto &ns: _namespaceStore | std::views::values) {
                if (ns.ern == ern) return ns;
            }
            return std::nullopt;
        }

        long countNamespaces(const std::string &accountId) const override {
            std::lock_guard lock(_mutex);
            return static_cast<long>(std::ranges::count_if(_namespaceStore | std::views::values, [&accountId](const auto &n) {
                return n.accountId == accountId;
            }));
        }

        std::vector<Entity::EAM::Namespace> listNamespaces(const std::string &accountId, const std::string &prefix, const long pageSize, const long pageIndex, const std::string &sortColumn, const std::string &sortDirection = "asc") const override {
            std::lock_guard lock(_mutex);

            std::vector<Entity::EAM::Namespace> namespaces;
            for (const auto &ns: _namespaceStore | std::views::values) {
                if (ns.accountId != accountId) continue;
                if (prefix.empty() || ns.name.starts_with(prefix)) namespaces.push_back(ns);
            }

            std::ranges::sort(namespaces, [&sortColumn, &sortDirection](const Entity::EAM::Namespace &a, const Entity::EAM::Namespace &b) {
                const bool ascending = sortDirection != "desc";
                if (sortColumn == "description") return ascending ? a.description < b.description : b.description < a.description;
                return ascending ? a.name < b.name : b.name < a.name;
            });

            if (pageSize > 0) {
                const auto offset = std::min<std::size_t>(static_cast<std::size_t>(std::max<long>(pageIndex, 0)) * pageSize, namespaces.size());
                const auto count = std::min<std::size_t>(static_cast<std::size_t>(pageSize), namespaces.size() - offset);
                namespaces = std::vector(namespaces.begin() + static_cast<std::ptrdiff_t>(offset), namespaces.begin() + static_cast<std::ptrdiff_t>(offset + count));
            }

            return namespaces;
        }

        void deleteNamespace(const std::string &accountId, const std::string &name) const override {
            std::lock_guard lock(_mutex);
            _namespaceStore.erase(namespaceKey(accountId, name));
        }

    private:

        // Namespace names repeat across accounts - only the (accountId, name) pair is unique, so
        // the store is keyed by a composite string rather than name alone.
        static std::string namespaceKey(const std::string &accountId, const std::string &name) {
            return accountId + "\x1f" + name;
        }

        mutable std::mutex _mutex;
        mutable std::unordered_map<std::string, Entity::EAM::User> _userStore;
        mutable std::unordered_map<std::string, Entity::EAM::UserGroup> _groupStore;
        mutable std::unordered_map<std::string, Entity::EAM::Account> _accountStore;
        mutable std::unordered_map<std::string, Entity::EAM::Namespace> _namespaceStore;
    };

}// namespace Euclid::Database