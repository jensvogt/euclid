//
// Created by vogje01 on 8/16/26.
//

#include <euclid/database/repository/eam/MongoEamRepository.h>

// C++ includes
#include <algorithm>

namespace Euclid::Database {

    MongoEamRepository::MongoEamRepository() {
        ensureIndexes();
    }

    void MongoEamRepository::ensureIndexes() {

        try {
            const auto entry = Database::instance().client();
            auto userCollection = (*entry)[Database::instance().databaseName()][USER_COLLECTION];

            mongocxx::options::index userIdOpts;
            userIdOpts.unique(true);
            userCollection.create_index(make_document(kvp("userId", 1)), userIdOpts);

            mongocxx::options::index emailOpts;
            emailOpts.unique(true);
            emailOpts.sparse(true);
            userCollection.create_index(make_document(kvp("email", 1)), emailOpts);

            // Resolves the caller on every SigV4-signed request to every module
            // (RepositoryFactory::WireAccessKeyLookup()) - the hottest lookup on this collection.
            mongocxx::options::index accessKeyIdOpts;
            accessKeyIdOpts.unique(true);
            accessKeyIdOpts.sparse(true);
            userCollection.create_index(make_document(kvp("accessKeys.accessKeyId", 1)), accessKeyIdOpts);

            auto userGroupCollection = (*entry)[Database::instance().databaseName()][USER_GROUP_COLLECTION];

            mongocxx::options::index groupNameOpts;
            groupNameOpts.unique(true);
            userGroupCollection.create_index(make_document(kvp("name", 1)), groupNameOpts);

            auto accountCollection = (*entry)[Database::instance().databaseName()][ACCOUNT_COLLECTION];

            mongocxx::options::index accountIdOpts;
            accountIdOpts.unique(true);
            accountCollection.create_index(make_document(kvp("accountId", 1)), accountIdOpts);

            auto namespaceCollection = (*entry)[Database::instance().databaseName()][NAMESPACE_COLLECTION];

            // Namespace names (e.g. "development") repeat across accounts - only the compound
            // (accountId, name) pair is unique.
            mongocxx::options::index namespaceOpts;
            namespaceOpts.unique(true);
            namespaceCollection.create_index(make_document(kvp("accountId", 1), kvp("name", 1)), namespaceOpts);

        } catch (const std::exception &e) {
            log_error << "Ensure user indexes failed, error: " << e.what();
        }
    }

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
            throw std::runtime_error("upsert returned no document, userId: " + user.userId);

        } catch (const std::exception &e) {
            log_error << "Upsert user failed, error: " << e.what();
            throw;
        }
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

    std::optional<Entity::EAM::User> MongoEamRepository::findUserByErn(const std::string &ern) const {

        try {

            const auto entry = Database::instance().client();
            auto userCollection = (*entry)[Database::instance().databaseName()][USER_COLLECTION];

            if (auto result = userCollection.find_one(make_document(kvp("ern", ern)))) {
                return Entity::EAM::User::fromDocument(result.value());
            }

        } catch (const std::exception &e) {
            log_error << "Get user by ERN failed, ern: " << ern << ", error: " << e.what();
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

    bool MongoEamRepository::userErnExists(const std::string &ern) const {

        try {

            const auto entry = Database::instance().client();
            auto userCollection = (*entry)[Database::instance().databaseName()][USER_COLLECTION];

            const auto result = userCollection.find_one(make_document(kvp("ern", ern)));
            return result.has_value();

        } catch (const std::exception &e) {
            log_error << "User exists failed, ern: " << ern << ", error: " << e.what();
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

    Entity::EAM::UserGroup MongoEamRepository::upsertUserGroup(Entity::EAM::UserGroup &group) {

        try {

            const auto filter = make_document(kvp("name", group.name));
            const auto update = make_document(kvp("$set", group.toDocument()));

            mongocxx::options::find_one_and_update opts;
            opts.upsert(true);
            opts.return_document(mongocxx::options::return_document::k_after);

            const auto entry = Database::instance().client();
            auto userGroupCollection = (*entry)[Database::instance().databaseName()][USER_GROUP_COLLECTION];

            if (auto result = userGroupCollection.find_one_and_update(filter.view(), update.view(), opts)) {
                return Entity::EAM::UserGroup::fromDocument(result->view());
            }
            throw std::runtime_error("upsert returned no document, name: " + group.name);

        } catch (const std::exception &e) {
            log_error << "Upsert user group failed, error: " << e.what();
            throw;
        }
    }

    bool MongoEamRepository::userGroupExists(const std::string &name) const {

        try {

            const auto entry = Database::instance().client();
            auto userGroupCollection = (*entry)[Database::instance().databaseName()][USER_GROUP_COLLECTION];

            const auto result = userGroupCollection.find_one(make_document(kvp("name", name)));
            return result.has_value();

        } catch (const std::exception &e) {
            log_error << "User group exists failed, name: " << name << ", error: " << e.what();
        }
        return false;
    }

    bool MongoEamRepository::userGroupErnExists(const std::string &ern) const {

        try {

            const auto entry = Database::instance().client();
            auto userGroupCollection = (*entry)[Database::instance().databaseName()][USER_GROUP_COLLECTION];

            const auto result = userGroupCollection.find_one(make_document(kvp("ern", ern)));
            return result.has_value();

        } catch (const std::exception &e) {
            log_error << "User group exists failed, ern: " << ern << ", error: " << e.what();
        }
        return false;
    }

    std::optional<Entity::EAM::UserGroup> MongoEamRepository::findUserGroupByName(const std::string &name) const {

        try {

            const auto entry = Database::instance().client();
            auto userGroupCollection = (*entry)[Database::instance().databaseName()][USER_GROUP_COLLECTION];

            if (auto result = userGroupCollection.find_one(make_document(kvp("name", name)))) {
                return Entity::EAM::UserGroup::fromDocument(result.value());
            }

        } catch (const std::exception &e) {
            log_error << "Get user group by name failed, name: " << name << ", error: " << e.what();
        }
        return {};
    }

    std::optional<Entity::EAM::UserGroup> MongoEamRepository::findUserGroupByErn(const std::string &ern) const {

        try {

            const auto entry = Database::instance().client();
            auto userGroupCollection = (*entry)[Database::instance().databaseName()][USER_GROUP_COLLECTION];

            if (auto result = userGroupCollection.find_one(make_document(kvp("ern", ern)))) {
                return Entity::EAM::UserGroup::fromDocument(result.value());
            }

        } catch (const std::exception &e) {
            log_error << "Get user group by ern failed, ern: " << ern << ", error: " << e.what();
        }
        return {};
    }

    long MongoEamRepository::countUserGroups() const {

        try {
            const auto entry = Database::instance().client();
            auto userCollection = (*entry)[Database::instance().databaseName()][USER_GROUP_COLLECTION];

            return static_cast<long>(userCollection.count_documents({}));
        } catch (const std::exception &e) {
            log_error << "Count users failed, error: " << e.what();
        }
        return -1;
    }

    std::vector<Entity::EAM::UserGroup> MongoEamRepository::listUserGroups(const std::string &prefix, const long pageSize, const long pageIndex, const std::string &sortColumn) const {

        std::vector<Entity::EAM::UserGroup> userGroups;
        try {

            const auto filter = prefix.empty() ? make_document() : make_document(kvp("name", make_document(kvp("$regex", "^" + prefix))));

            mongocxx::options::find opts;
            if (!sortColumn.empty()) {
                opts.sort(make_document(kvp(sortColumn, 1)));
            }
            if (pageSize > 0) {
                opts.limit(pageSize);
                opts.skip(std::max<long>(pageIndex, 0) * pageSize);
            }

            const auto entry = Database::instance().client();
            auto userGroupsCollection = (*entry)[Database::instance().databaseName()][USER_GROUP_COLLECTION];

            for (auto cursor = userGroupsCollection.find(filter.view(), opts); auto doc: cursor) {
                userGroups.push_back(Entity::EAM::UserGroup::fromDocument(doc));
            }

        } catch (const std::exception &e) {
            log_error << "List userGroups failed, error: " << e.what();
        }
        return userGroups;
    }

    void MongoEamRepository::deleteUserGroup(const std::string &name) const {

        try {
            const auto entry = Database::instance().client();
            auto userGroupCollection = (*entry)[Database::instance().databaseName()][USER_GROUP_COLLECTION];

            const auto result = userGroupCollection.delete_many(make_document(kvp("name", name)));
            log_debug << "User group deleted, count: " << result->deleted_count();

        } catch (const std::exception &e) {
            log_error << "Delete user group failed, name: " << name << ", error: " << e.what();
        }
    }

    Entity::EAM::Account MongoEamRepository::upsertAccount(Entity::EAM::Account &account) {

        try {

            const auto filter = make_document(kvp("accountId", account.accountId));
            const auto update = make_document(kvp("$set", account.toDocument()));

            mongocxx::options::find_one_and_update opts;
            opts.upsert(true);
            opts.return_document(mongocxx::options::return_document::k_after);

            const auto entry = Database::instance().client();
            auto accountCollection = (*entry)[Database::instance().databaseName()][ACCOUNT_COLLECTION];

            if (auto result = accountCollection.find_one_and_update(filter.view(), update.view(), opts)) {
                return Entity::EAM::Account::fromDocument(result->view());
            }
            throw std::runtime_error("upsert returned no document, accountId: " + account.accountId);

        } catch (const std::exception &e) {
            log_error << "Upsert account failed, error: " << e.what();
            throw;
        }
    }

    bool MongoEamRepository::accountExists(const std::string &accountId) const {

        try {

            const auto entry = Database::instance().client();
            auto accountCollection = (*entry)[Database::instance().databaseName()][ACCOUNT_COLLECTION];

            const auto result = accountCollection.find_one(make_document(kvp("accountId", accountId)));
            return result.has_value();

        } catch (const std::exception &e) {
            log_error << "Account exists failed, accountId: " << accountId << ", error: " << e.what();
        }
        return false;
    }

    bool MongoEamRepository::accountErnExists(const std::string &ern) const {

        try {

            const auto entry = Database::instance().client();
            auto accountCollection = (*entry)[Database::instance().databaseName()][ACCOUNT_COLLECTION];

            const auto result = accountCollection.find_one(make_document(kvp("ern", ern)));
            return result.has_value();

        } catch (const std::exception &e) {
            log_error << "Account exists failed, ern: " << ern << ", error: " << e.what();
        }
        return false;
    }

    std::optional<Entity::EAM::Account> MongoEamRepository::findAccountByAccountId(const std::string &accountId) const {

        try {

            const auto entry = Database::instance().client();
            auto accountCollection = (*entry)[Database::instance().databaseName()][ACCOUNT_COLLECTION];

            if (auto result = accountCollection.find_one(make_document(kvp("accountId", accountId)))) {
                return Entity::EAM::Account::fromDocument(result.value());
            }

        } catch (const std::exception &e) {
            log_error << "Get account by accountId failed, accountId: " << accountId << ", error: " << e.what();
        }
        return {};
    }

    std::optional<Entity::EAM::Account> MongoEamRepository::findAccountByErn(const std::string &ern) const {

        try {

            const auto entry = Database::instance().client();
            auto accountCollection = (*entry)[Database::instance().databaseName()][ACCOUNT_COLLECTION];

            if (auto result = accountCollection.find_one(make_document(kvp("ern", ern)))) {
                return Entity::EAM::Account::fromDocument(result.value());
            }

        } catch (const std::exception &e) {
            log_error << "Get account by ERN failed, ern: " << ern << ", error: " << e.what();
        }
        return {};
    }

    long MongoEamRepository::countAccounts() const {

        try {
            const auto entry = Database::instance().client();
            auto accountCollection = (*entry)[Database::instance().databaseName()][ACCOUNT_COLLECTION];

            return static_cast<long>(accountCollection.count_documents({}));
        } catch (const std::exception &e) {
            log_error << "Count accounts failed, error: " << e.what();
        }
        return -1;
    }

    std::vector<Entity::EAM::Account> MongoEamRepository::listAccounts(const std::string &prefix, const long pageSize, const long pageIndex, const std::string &sortColumn) const {

        std::vector<Entity::EAM::Account> accounts;
        try {

            const auto filter = prefix.empty() ? make_document() : make_document(kvp("accountId", make_document(kvp("$regex", "^" + prefix))));

            mongocxx::options::find opts;
            if (!sortColumn.empty()) {
                opts.sort(make_document(kvp(sortColumn, 1)));
            }
            if (pageSize > 0) {
                opts.limit(pageSize);
                opts.skip(std::max<long>(pageIndex, 0) * pageSize);
            }

            const auto entry = Database::instance().client();
            auto accountCollection = (*entry)[Database::instance().databaseName()][ACCOUNT_COLLECTION];

            for (auto cursor = accountCollection.find(filter.view(), opts); auto doc: cursor) {
                accounts.push_back(Entity::EAM::Account::fromDocument(doc));
            }

        } catch (const std::exception &e) {
            log_error << "List accounts failed, error: " << e.what();
        }
        return accounts;
    }

    void MongoEamRepository::deleteAccount(const std::string &accountId) const {

        try {
            const auto entry = Database::instance().client();
            auto accountCollection = (*entry)[Database::instance().databaseName()][ACCOUNT_COLLECTION];

            const auto result = accountCollection.delete_many(make_document(kvp("accountId", accountId)));
            log_debug << "Account deleted, count: " << result->deleted_count();

        } catch (const std::exception &e) {
            log_error << "Delete account failed, accountId: " << accountId << ", error: " << e.what();
        }
    }

    Entity::EAM::Namespace MongoEamRepository::upsertNamespace(Entity::EAM::Namespace &ns) {

        try {

            const auto filter = make_document(kvp("accountId", ns.accountId), kvp("name", ns.name));
            const auto update = make_document(kvp("$set", ns.toDocument()));

            mongocxx::options::find_one_and_update opts;
            opts.upsert(true);
            opts.return_document(mongocxx::options::return_document::k_after);

            const auto entry = Database::instance().client();
            auto namespaceCollection = (*entry)[Database::instance().databaseName()][NAMESPACE_COLLECTION];

            if (auto result = namespaceCollection.find_one_and_update(filter.view(), update.view(), opts)) {
                return Entity::EAM::Namespace::fromDocument(result->view());
            }
            throw std::runtime_error("upsert returned no document, name: " + ns.name);

        } catch (const std::exception &e) {
            log_error << "Upsert namespace failed, error: " << e.what();
            throw;
        }
    }

    bool MongoEamRepository::namespaceExists(const std::string &accountId, const std::string &name) const {

        try {

            const auto entry = Database::instance().client();
            auto namespaceCollection = (*entry)[Database::instance().databaseName()][NAMESPACE_COLLECTION];

            const auto result = namespaceCollection.find_one(make_document(kvp("accountId", accountId), kvp("name", name)));
            return result.has_value();

        } catch (const std::exception &e) {
            log_error << "Namespace exists failed, accountId: " << accountId << ", name: " << name << ", error: " << e.what();
        }
        return false;
    }

    bool MongoEamRepository::namespaceErnExists(const std::string &ern) const {

        try {

            const auto entry = Database::instance().client();
            auto namespaceCollection = (*entry)[Database::instance().databaseName()][NAMESPACE_COLLECTION];

            const auto result = namespaceCollection.find_one(make_document(kvp("ern", ern)));
            return result.has_value();

        } catch (const std::exception &e) {
            log_error << "Namespace exists failed, ern: " << ern << ", error: " << e.what();
        }
        return false;
    }

    std::optional<Entity::EAM::Namespace> MongoEamRepository::findNamespaceByName(const std::string &accountId, const std::string &name) const {

        try {

            const auto entry = Database::instance().client();
            auto namespaceCollection = (*entry)[Database::instance().databaseName()][NAMESPACE_COLLECTION];

            if (auto result = namespaceCollection.find_one(make_document(kvp("accountId", accountId), kvp("name", name)))) {
                return Entity::EAM::Namespace::fromDocument(result.value());
            }

        } catch (const std::exception &e) {
            log_error << "Get namespace by name failed, accountId: " << accountId << ", name: " << name << ", error: " << e.what();
        }
        return {};
    }

    std::optional<Entity::EAM::Namespace> MongoEamRepository::findNamespaceByErn(const std::string &ern) const {

        try {

            const auto entry = Database::instance().client();
            auto namespaceCollection = (*entry)[Database::instance().databaseName()][NAMESPACE_COLLECTION];

            if (auto result = namespaceCollection.find_one(make_document(kvp("ern", ern)))) {
                return Entity::EAM::Namespace::fromDocument(result.value());
            }

        } catch (const std::exception &e) {
            log_error << "Get namespace by ern failed, ern: " << ern << ", error: " << e.what();
        }
        return {};
    }

    long MongoEamRepository::countNamespaces(const std::string &accountId) const {

        try {
            const auto entry = Database::instance().client();
            auto namespaceCollection = (*entry)[Database::instance().databaseName()][NAMESPACE_COLLECTION];

            return static_cast<long>(namespaceCollection.count_documents(make_document(kvp("accountId", accountId))));
        } catch (const std::exception &e) {
            log_error << "Count namespaces failed, accountId: " << accountId << ", error: " << e.what();
        }
        return -1;
    }

    std::vector<Entity::EAM::Namespace> MongoEamRepository::listNamespaces(const std::string &accountId, const std::string &prefix, const long pageSize, const long pageIndex, const std::string &sortColumn) const {

        std::vector<Entity::EAM::Namespace> namespaces;
        try {

            auto filter = prefix.empty()
                    ? make_document(kvp("accountId", accountId))
                    : make_document(kvp("accountId", accountId), kvp("name", make_document(kvp("$regex", "^" + prefix))));

            mongocxx::options::find opts;
            if (!sortColumn.empty()) {
                opts.sort(make_document(kvp(sortColumn, 1)));
            }
            if (pageSize > 0) {
                opts.limit(pageSize);
                opts.skip(std::max<long>(pageIndex, 0) * pageSize);
            }

            const auto entry = Database::instance().client();
            auto namespaceCollection = (*entry)[Database::instance().databaseName()][NAMESPACE_COLLECTION];

            for (auto cursor = namespaceCollection.find(filter.view(), opts); auto doc: cursor) {
                namespaces.push_back(Entity::EAM::Namespace::fromDocument(doc));
            }

        } catch (const std::exception &e) {
            log_error << "List namespaces failed, accountId: " << accountId << ", error: " << e.what();
        }
        return namespaces;
    }

    void MongoEamRepository::deleteNamespace(const std::string &accountId, const std::string &name) const {

        try {
            const auto entry = Database::instance().client();
            auto namespaceCollection = (*entry)[Database::instance().databaseName()][NAMESPACE_COLLECTION];

            const auto result = namespaceCollection.delete_many(make_document(kvp("accountId", accountId), kvp("name", name)));
            log_debug << "Namespace deleted, count: " << result->deleted_count();

        } catch (const std::exception &e) {
            log_error << "Delete namespace failed, accountId: " << accountId << ", name: " << name << ", error: " << e.what();
        }
    }

}// namespace Euclid::Database