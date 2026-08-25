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
         * @brief Returns the number of existing users.
         *
         * @return number of existing users.
         */
        [[nodiscard]]
        long countUsers() const override;

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
        std::vector<Entity::EAM::User> listUsers(const std::string &prefix, long pageSize, long pageIndex, const std::string &sortColumn) const override;

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
        std::vector<Entity::EAM::UserGroup> listUserGroups(const std::string &prefix, long pageSize, long pageIndex, const std::string &sortColumn) const override;

        /**
         * @brief Removes a user group by its name.
         *
         * @param name The name of the group to be removed.
         */
        void deleteUserGroup(const std::string &name) const override;

    private:

        static constexpr auto USER_COLLECTION = "eam_user";
        static constexpr auto USER_GROUP_COLLECTION = "eam_usergroup";

        /**
         * @brief Creates the indexes required for efficient user lookup, if they do not already exist.
         */
        static void ensureIndexes();
    };

}// namespace Euclid::Database