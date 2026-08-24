//
// Created by vogje01 on 8/16/26.
//

#include <euclid/database/repository/eam/MongoEamRepository.h>

// C++ includes
#include <algorithm>

namespace Euclid::Database {

    Entity::EAM::User MongoEamRepository::upsertUser(Entity::EAM::User &user) {

        try {

            const auto filter = make_document(kvp("userId", user.userId));
            const auto update = make_document(kvp("$set", user.toDocument()));

            mongocxx::options::find_one_and_update opts;
            opts.upsert(true);
            opts.return_document(mongocxx::options::return_document::k_after);

            const auto entry = Database::instance().client();
            auto userCollection = (*entry)[Database::instance().databaseName()][USER_COLLECTION];

            if (auto result = userCollection.find_one_and_update(filter.view(), update.view(), opts)) {
                return Entity::EAM::User::fromDocument(result->view());
            }

        } catch (const std::exception &e) {
            log_error << "Upsert user failed, error: " << e.what();
        }
        return user;
    }

    std::optional<Entity::EAM::User> MongoEamRepository::findUserByUserId(const std::string &userId) const {

        try {

            const auto entry = Database::instance().client();
            auto userCollection = (*entry)[Database::instance().databaseName()][USER_COLLECTION];

            if (auto result = userCollection.find_one(make_document(kvp("userId", userId)))) {
                return Entity::EAM::User::fromDocument(result.value());
            }

        } catch (const std::exception &e) {
            log_error << "Get user by userId failed, userId: " << userId << ", error: " << e.what();
        }
        return {};
    }

    std::optional<Entity::EAM::User> MongoEamRepository::findUserByEmail(const std::string &email) const {

        try {

            const auto entry = Database::instance().client();
            auto userCollection = (*entry)[Database::instance().databaseName()][USER_COLLECTION];

            if (auto result = userCollection.find_one(make_document(kvp("email", email)))) {
                return Entity::EAM::User::fromDocument(result.value());
            }

        } catch (const std::exception &e) {
            log_error << "Get user by email failed, email: " << email << ", error: " << e.what();
        }
        return {};
    }

    std::optional<Entity::EAM::User> MongoEamRepository::findUserByAccessKeyId(const std::string &accessKeyId) const {

        try {

            const auto entry = Database::instance().client();
            auto userCollection = (*entry)[Database::instance().databaseName()][USER_COLLECTION];

            if (auto result = userCollection.find_one(make_document(kvp("accessKeys.accessKeyId", accessKeyId)))) {
                return Entity::EAM::User::fromDocument(result.value());
            }

        } catch (const std::exception &e) {
            log_error << "Get user by accessKeyId failed, accessKeyId: " << accessKeyId << ", error: " << e.what();
        }
        return {};
    }

    bool MongoEamRepository::userExists(const std::string &userId) const {

        try {

            const auto entry = Database::instance().client();
            auto userCollection = (*entry)[Database::instance().databaseName()][USER_COLLECTION];

            const auto result = userCollection.find_one(make_document(kvp("userId", userId)));
            return result.has_value();

        } catch (const std::exception &e) {
            log_error << "User exists failed, userId: " << userId << ", error: " << e.what();
        }
        return false;
    }

    long MongoEamRepository::countUsers() const {

        try {
            const auto entry = Database::instance().client();
            auto userCollection = (*entry)[Database::instance().databaseName()][USER_COLLECTION];

            return userCollection.count_documents({});
        } catch (const std::exception &e) {
            log_error << "Count users failed, error: " << e.what();
        }
        return -1;
    }

    std::vector<Entity::EAM::User> MongoEamRepository::listUsers(const std::string &prefix, const long pageSize, const long pageIndex, const std::string &sortColumn) const {

        std::vector<Entity::EAM::User> users;
        try {

            const auto filter = prefix.empty() ? make_document() : make_document(kvp("userId", make_document(kvp("$regex", "^" + prefix))));

            mongocxx::options::find opts;
            if (!sortColumn.empty()) {
                opts.sort(make_document(kvp(sortColumn, 1)));
            }
            if (pageSize > 0) {
                opts.limit(pageSize);
                opts.skip(std::max<long>(pageIndex, 0) * pageSize);
            }

            const auto entry = Database::instance().client();
            auto userCollection = (*entry)[Database::instance().databaseName()][USER_COLLECTION];

            for (auto cursor = userCollection.find(filter.view(), opts); auto doc: cursor) {
                users.push_back(Entity::EAM::User::fromDocument(doc));
            }

        } catch (const std::exception &e) {
            log_error << "List users failed, error: " << e.what();
        }
        return users;
    }

    void MongoEamRepository::deleteUser(const std::string &userId) const {

        try {
            const auto entry = Database::instance().client();
            auto userCollection = (*entry)[Database::instance().databaseName()][USER_COLLECTION];

            const auto result = userCollection.delete_many(make_document(kvp("userId", userId)));
            log_debug << "User deleted, count: " << result->deleted_count();

        } catch (const std::exception &e) {
            log_error << "Delete user failed, userId: " << userId << ", error: " << e.what();
        }
    }
}// namespace Euclid::Database