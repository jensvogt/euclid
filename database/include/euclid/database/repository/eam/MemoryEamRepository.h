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

        long countUsers() const override {
            std::lock_guard lock(_mutex);
            return static_cast<long>(_userStore.size());
        }

        std::vector<Entity::EAM::User> listUsers(const std::string &prefix, const long pageSize, const long pageIndex, const std::string &sortColumn) const override {
            std::lock_guard lock(_mutex);

            std::vector<Entity::EAM::User> users;
            for (const auto &user: _userStore | std::views::values) {
                if (prefix.empty() || user.userId.starts_with(prefix)) users.push_back(user);
            }

            std::ranges::sort(users, [&sortColumn](const Entity::EAM::User &a, const Entity::EAM::User &b) {
                if (sortColumn == "email") return a.email < b.email;
                if (sortColumn == "isAdmin") return a.isAdmin < b.isAdmin;
                return a.userId < b.userId;
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

        long countUserGroups() const override {
            std::lock_guard lock(_mutex);
            return static_cast<long>(_groupStore.size());
        }

        std::vector<Entity::EAM::UserGroup> listUserGroups(const std::string &prefix, const long pageSize, const long pageIndex, const std::string &sortColumn) const override {
            std::lock_guard lock(_mutex);

            std::vector<Entity::EAM::UserGroup> userGroups;
            for (const auto &userGroup: _groupStore | std::views::values) {
                if (prefix.empty() || userGroup.name.starts_with(prefix)) userGroups.push_back(userGroup);
            }

            std::ranges::sort(userGroups, [&sortColumn](const Entity::EAM::UserGroup &a, const Entity::EAM::UserGroup &b) {
                if (sortColumn == "name") return a.name < b.name;
                if (sortColumn == "description") return a.description < b.description;
                return a.name < b.name;
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

    private:

        mutable std::mutex _mutex;
        mutable std::unordered_map<std::string, Entity::EAM::User> _userStore;
        mutable std::unordered_map<std::string, Entity::EAM::UserGroup> _groupStore;
    };

}// namespace Euclid::Database
