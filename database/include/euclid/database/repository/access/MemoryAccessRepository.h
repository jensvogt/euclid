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
#include <euclid/database/entity/access/User.h>
#include <euclid/database/repository/access/IAccessRepository.h>

namespace Euclid::Database {

    /**
     * @brief Access in-memory database.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class MemoryAccessRepository final : public IAccessRepository {

    public:

        /**
         * @brief Singleton instance
         */
        static MemoryAccessRepository &instance() {
            static MemoryAccessRepository accessDatabase;
            return accessDatabase;
        }

        Entity::Access::User upsertUser(Entity::Access::User &user) override {
            std::lock_guard lock(_mutex);
            if (user.oid.empty()) user.oid = Core::UuidUtils::CreateRandomUuid();
            _userStore[user.userId] = user;
            return user;
        }

        void deleteUser(const std::string &userId) const override {
            std::lock_guard lock(_mutex);
            _userStore.erase(userId);
        }

        std::optional<Entity::Access::User> findUserByUserId(const std::string &userId) const override {
            std::lock_guard lock(_mutex);
            const auto it = _userStore.find(userId);
            if (it == _userStore.end()) return std::nullopt;
            return it->second;
        }

        std::optional<Entity::Access::User> findUserByEmail(const std::string &email) const override {
            std::lock_guard lock(_mutex);
            for (const auto &user: _userStore | std::views::values) {
                if (user.email == email) return user;
            }
            return std::nullopt;
        }

        std::optional<Entity::Access::User> findUserByAccessKeyId(const std::string &accessKeyId) const override {
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

        std::vector<Entity::Access::User> listUsers(const std::string &prefix, const long pageSize, const long pageIndex, const std::string &sortColumn) const override {
            std::lock_guard lock(_mutex);

            std::vector<Entity::Access::User> users;
            for (const auto &user: _userStore | std::views::values) {
                if (prefix.empty() || user.userId.starts_with(prefix)) users.push_back(user);
            }

            std::ranges::sort(users, [&sortColumn](const Entity::Access::User &a, const Entity::Access::User &b) {
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

    private:

        mutable std::mutex _mutex;
        mutable std::unordered_map<std::string, Entity::Access::User> _userStore;
    };

}// namespace Euclid::Database
