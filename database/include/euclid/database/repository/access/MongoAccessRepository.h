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
#include <euclid/database/entity/access/User.h>
#include <euclid/database/repository/access/IAccessRepository.h>

namespace Euclid::Database {

    using namespace bsoncxx::builder::basic;

    /**
     * @brief Access MongoDB database.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class MongoAccessRepository final : public IAccessRepository {

    public:

        /**
         * @brief Constructor
         */
        explicit MongoAccessRepository() : _databaseName("euclid") {}

        /**
         * @brief Singleton instance
         */
        static MongoAccessRepository &instance() {
            static MongoAccessRepository accessDatabase;
            return accessDatabase;
        }

        Entity::Access::User upsertUser(Entity::Access::User &user) override;

        [[nodiscard]]
        std::optional<Entity::Access::User> findUserByUserId(const std::string &userId) const override;

        [[nodiscard]]
        std::optional<Entity::Access::User> findUserByEmail(const std::string &email) const override;

        [[nodiscard]]
        std::optional<Entity::Access::User> findUserByAccessKeyId(const std::string &accessKeyId) const override;

        [[nodiscard]]
        bool userExists(const std::string &userId) const override;

        [[nodiscard]]
        long countUsers() const override;

        [[nodiscard]]
        std::vector<Entity::Access::User> listUsers(const std::string &prefix, long pageSize, long pageIndex, const std::string &sortColumn) const override;

        void deleteUser(const std::string &userId) const override;

    private:

        static constexpr auto USER_COLLECTION = "access_user";

        /**
         * Database name
         */
        std::string _databaseName;
    };

}// namespace Euclid::Database
