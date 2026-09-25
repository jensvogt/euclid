// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/16/26.
//

#include <euclid/database/repository/eam/MongoEamRepository.h>

// C++ includes
#include <algorithm>

// Euclid includes
#include <euclid/core/ErnUtils.h>

namespace Euclid::Database {

    MongoEamRepository::MongoEamRepository() {
        ensureIndexes();
    }

    void MongoEamRepository::ensureIndexes() {

        try {
            auto userCollection = Database::instance().collection(USER_COLLECTION);

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

            auto userGroupCollection = Database::instance().collection(USER_GROUP_COLLECTION);

            mongocxx::options::index groupNameOpts;
            groupNameOpts.unique(true);
            userGroupCollection.create_index(make_document(kvp("name", 1)), groupNameOpts);

            auto accountCollection = Database::instance().collection(ACCOUNT_COLLECTION);

            mongocxx::options::index accountIdOpts;
            accountIdOpts.unique(true);
            accountCollection.create_index(make_document(kvp("accountId", 1)), accountIdOpts);

            auto namespaceCollection = Database::instance().collection(NAMESPACE_COLLECTION);

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

        // Filled in when the caller left them unset, rather than required of every caller. Every
        // handler in EAM stamps them and EAP's technical principal did not, so every application's
        // identity was stored as having been created at the epoch - and nothing said so, because a
        // date that is wrong still sorts, still formats and still prints.
        //
        // Only when unset: a caller that says when something happened is not corrected, and an
        // update does not become a creation. A row written before this, carrying the epoch, is
        // stamped the next time anything writes it - the honest answer to "when was this made" is
        // then "not known, first seen now", which is what it says.
        if (const auto now = std::chrono::system_clock::now(); user.created.time_since_epoch().count() == 0) {
            user.created = now;
            if (user.modified.time_since_epoch().count() == 0) user.modified = now;
        } else if (user.modified.time_since_epoch().count() == 0) {
            user.modified = now;
        }

        try {

            const auto filter = make_document(kvp("userId", user.userId));
            const auto update = make_document(kvp("$set", user.toDocument()));

            mongocxx::options::find_one_and_update opts;
            opts.upsert(true);
            opts.return_document(mongocxx::options::return_document::k_after);

            auto userCollection = Database::instance().collection(USER_COLLECTION);

            if (auto result = userCollection.find_one_and_update(filter.view(), update.view(), opts)) {
                return Entity::EAM::User::fromDocument(result->view());
            }
            throw std::runtime_error("upsert returned no document, userId: " + user.userId);

        } catch (const std::exception &e) {
            log_error << "Upsert user failed, error: " << e.what();
            throw;
        }
    }

    std::optional<Entity::EAM::User> MongoEamRepository::renameUser(const std::string &userId, const std::string &newUserId) {

        const auto existing = findUserByUserId(userId);
        if (!existing.has_value()) return std::nullopt;

        // Rebuilt rather than patched: an ERN carries the region and the account as well as the
        // name, and the one this user holds was built the same way when they were created.
        const auto newErn = Core::createEamUserErn(existing->accountId, newUserId);

        try {

            // The user row first. If this is refused - the unique index on userId, because the new
            // name is taken - nothing else has moved yet, and the user is exactly as they were.
            const auto filter = make_document(kvp("userId", userId));
            const auto update = make_document(kvp("$set", make_document(kvp("userId", newUserId), kvp("ern", newErn))));

            mongocxx::options::find_one_and_update opts;
            opts.return_document(mongocxx::options::return_document::k_after);

            auto userCollection = Database::instance().collection(USER_COLLECTION);
            const auto result = userCollection.find_one_and_update(filter.view(), update.view(), opts);
            if (!result.has_value()) return std::nullopt;

            // Then the grants, which hang off the ERN and are the whole of what this user may do.
            // One statement rather than a read-modify-write per grant: nothing here needs to see
            // them, and a principal that changed under a partial rewrite would hold half its rights.
            auto grantCollection = Database::instance().collection(GRANT_COLLECTION);
            const auto grantFilter = make_document(kvp("principal", existing->ern));
            const auto grantUpdate = make_document(kvp("$set", make_document(kvp("principal", newErn))));
            const auto movedGrants = grantCollection.update_many(grantFilter.view(), grantUpdate.view());

            // And the group memberships, which are lists of userIds rather than of ERNs - so a
            // rename drops the user out of every group they were in unless this follows. The
            // groups are few and installation-wide (see PrincipalsOf), so they are read and
            // rewritten one at a time rather than with an array update the memory backend would
            // have to grow support for.
            long movedMemberships = 0;
            for (auto group: listUserGroups("", 0, 0, "name")) {
                const auto member = std::ranges::find(group.userIds, userId);
                if (member == group.userIds.end()) continue;

                *member = newUserId;
                std::ignore = upsertUserGroup(group);
                movedMemberships++;
            }

            log_info << "User renamed, userId: " << userId << " -> " << newUserId
                     << ", grants: " << (movedGrants.has_value() ? movedGrants->modified_count() : 0)
                     << ", groups: " << movedMemberships;

            return Entity::EAM::User::fromDocument(result->view());

        } catch (const std::exception &e) {
            // Rethrown rather than swallowed into a nullopt, which the caller reads as "no such
            // user" - a rename that half happened is not a missing user, and the difference decides
            // whether an operator goes looking.
            log_error << "Rename user failed, userId: " << userId << " -> " << newUserId << ", error: " << e.what();
            throw;
        }
    }

    std::optional<Entity::EAM::User> MongoEamRepository::findUserByUserId(const std::string &userId) const {

        try {

            auto userCollection = Database::instance().collection(USER_COLLECTION);

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

            auto userCollection = Database::instance().collection(USER_COLLECTION);

            if (auto result = userCollection.find_one(make_document(kvp("email", email)))) {
                return Entity::EAM::User::fromDocument(result.value());
            }

        } catch (const std::exception &e) {
            log_error << "Get user by email failed, email: " << email << ", error: " << e.what();
        }
        return {};
    }

    std::optional<Entity::EAM::User> MongoEamRepository::findUserByFederatedSubject(const std::string &provider, const std::string &subject) const {

        // Refused rather than looked up: every password user carries an empty provider and subject,
        // so an empty search value would match one of them arbitrarily and hand a federated login
        // somebody else's account.
        if (provider.empty() || subject.empty()) return {};

        try {

            auto userCollection = Database::instance().collection(USER_COLLECTION);

            if (auto result = userCollection.find_one(make_document(kvp("federatedProvider", provider), kvp("federatedSubject", subject)))) {
                return Entity::EAM::User::fromDocument(result.value());
            }

        } catch (const std::exception &e) {
            log_error << "Get user by federated subject failed, provider: " << provider << ", subject: " << subject << ", error: " << e.what();
        }
        return {};
    }

    std::optional<Entity::EAM::User> MongoEamRepository::findUserByErn(const std::string &ern) const {

        try {

            auto userCollection = Database::instance().collection(USER_COLLECTION);

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

            auto userCollection = Database::instance().collection(USER_COLLECTION);

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

            auto userCollection = Database::instance().collection(USER_COLLECTION);

            const auto result = userCollection.find_one(make_document(kvp("userId", userId)));
            return result.has_value();

        } catch (const std::exception &e) {
            log_error << "User exists failed, userId: " << userId << ", error: " << e.what();
        }
        return false;
    }

    bool MongoEamRepository::userErnExists(const std::string &ern) const {

        try {

            auto userCollection = Database::instance().collection(USER_COLLECTION);

            const auto result = userCollection.find_one(make_document(kvp("ern", ern)));
            return result.has_value();

        } catch (const std::exception &e) {
            log_error << "User exists failed, ern: " << ern << ", error: " << e.what();
        }
        return false;
    }

    long MongoEamRepository::countUsers() const {

        try {
            auto userCollection = Database::instance().collection(USER_COLLECTION);

            return userCollection.count_documents({});
        } catch (const std::exception &e) {
            log_error << "Count users failed, error: " << e.what();
        }
        return -1;
    }

    std::vector<Entity::EAM::User> MongoEamRepository::listUsers(const std::string &prefix, const long pageSize, const long pageIndex, const std::string &sortColumn, const std::string &sortDirection) const {

        std::vector<Entity::EAM::User> users;
        try {

            const auto filter = prefix.empty() ? make_document() : make_document(kvp("userId", make_document(kvp("$regex", "^" + prefix))));

            mongocxx::options::find opts;
            if (!sortColumn.empty()) {
                opts.sort(make_document(kvp(sortColumn, sortDirection == "asc" ? 1 : -1)));
            }
            if (pageSize > 0) {
                opts.limit(pageSize);
                opts.skip(std::max<long>(pageIndex, 0) * pageSize);
            }

            auto userCollection = Database::instance().collection(USER_COLLECTION);

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
            auto userCollection = Database::instance().collection(USER_COLLECTION);

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

            auto userGroupCollection = Database::instance().collection(USER_GROUP_COLLECTION);

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

            auto userGroupCollection = Database::instance().collection(USER_GROUP_COLLECTION);

            const auto result = userGroupCollection.find_one(make_document(kvp("name", name)));
            return result.has_value();

        } catch (const std::exception &e) {
            log_error << "User group exists failed, name: " << name << ", error: " << e.what();
        }
        return false;
    }

    bool MongoEamRepository::userGroupErnExists(const std::string &ern) const {

        try {

            auto userGroupCollection = Database::instance().collection(USER_GROUP_COLLECTION);

            const auto result = userGroupCollection.find_one(make_document(kvp("ern", ern)));
            return result.has_value();

        } catch (const std::exception &e) {
            log_error << "User group exists failed, ern: " << ern << ", error: " << e.what();
        }
        return false;
    }

    std::optional<Entity::EAM::UserGroup> MongoEamRepository::findUserGroupByName(const std::string &name) const {

        try {

            auto userGroupCollection = Database::instance().collection(USER_GROUP_COLLECTION);

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

            auto userGroupCollection = Database::instance().collection(USER_GROUP_COLLECTION);

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
            auto userCollection = Database::instance().collection(USER_GROUP_COLLECTION);

            return static_cast<long>(userCollection.count_documents({}));
        } catch (const std::exception &e) {
            log_error << "Count users failed, error: " << e.what();
        }
        return -1;
    }

    std::vector<Entity::EAM::UserGroup> MongoEamRepository::listUserGroups(const std::string &prefix, const long pageSize, const long pageIndex, const std::string &sortColumn, const std::string &sortDirection) const {

        std::vector<Entity::EAM::UserGroup> userGroups;
        try {

            const auto filter = prefix.empty() ? make_document() : make_document(kvp("name", make_document(kvp("$regex", "^" + prefix))));

            mongocxx::options::find opts;
            if (!sortColumn.empty()) {
                opts.sort(make_document(kvp(sortColumn, sortDirection == "asc" ? 1 : -1)));
            }
            if (pageSize > 0) {
                opts.limit(pageSize);
                opts.skip(std::max<long>(pageIndex, 0) * pageSize);
            }

            auto userGroupsCollection = Database::instance().collection(USER_GROUP_COLLECTION);

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
            auto userGroupCollection = Database::instance().collection(USER_GROUP_COLLECTION);

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

            auto accountCollection = Database::instance().collection(ACCOUNT_COLLECTION);

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

            auto accountCollection = Database::instance().collection(ACCOUNT_COLLECTION);

            const auto result = accountCollection.find_one(make_document(kvp("accountId", accountId)));
            return result.has_value();

        } catch (const std::exception &e) {
            log_error << "Account exists failed, accountId: " << accountId << ", error: " << e.what();
        }
        return false;
    }

    bool MongoEamRepository::accountErnExists(const std::string &ern) const {

        try {

            auto accountCollection = Database::instance().collection(ACCOUNT_COLLECTION);

            const auto result = accountCollection.find_one(make_document(kvp("ern", ern)));
            return result.has_value();

        } catch (const std::exception &e) {
            log_error << "Account exists failed, ern: " << ern << ", error: " << e.what();
        }
        return false;
    }

    std::optional<Entity::EAM::Account> MongoEamRepository::findAccountByAccountId(const std::string &accountId) const {

        try {

            auto accountCollection = Database::instance().collection(ACCOUNT_COLLECTION);

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

            auto accountCollection = Database::instance().collection(ACCOUNT_COLLECTION);

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
            auto accountCollection = Database::instance().collection(ACCOUNT_COLLECTION);

            return static_cast<long>(accountCollection.count_documents({}));
        } catch (const std::exception &e) {
            log_error << "Count accounts failed, error: " << e.what();
        }
        return -1;
    }

    std::vector<Entity::EAM::Account> MongoEamRepository::listAccounts(const std::string &prefix, const long pageSize, const long pageIndex, const std::string &sortColumn, const std::string &sortDirection) const {

        std::vector<Entity::EAM::Account> accounts;
        try {

            const auto filter = prefix.empty() ? make_document() : make_document(kvp("accountId", make_document(kvp("$regex", "^" + prefix))));

            mongocxx::options::find opts;
            if (!sortColumn.empty()) {
                opts.sort(make_document(kvp(sortColumn, sortDirection == "asc" ? 1 : -1)));
            }
            if (pageSize > 0) {
                opts.limit(pageSize);
                opts.skip(std::max<long>(pageIndex, 0) * pageSize);
            }

            auto accountCollection = Database::instance().collection(ACCOUNT_COLLECTION);

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
            auto accountCollection = Database::instance().collection(ACCOUNT_COLLECTION);

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

            auto namespaceCollection = Database::instance().collection(NAMESPACE_COLLECTION);

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

            auto namespaceCollection = Database::instance().collection(NAMESPACE_COLLECTION);

            const auto result = namespaceCollection.find_one(make_document(kvp("accountId", accountId), kvp("name", name)));
            return result.has_value();

        } catch (const std::exception &e) {
            log_error << "Namespace exists failed, accountId: " << accountId << ", name: " << name << ", error: " << e.what();
        }
        return false;
    }

    bool MongoEamRepository::namespaceErnExists(const std::string &ern) const {

        try {

            auto namespaceCollection = Database::instance().collection(NAMESPACE_COLLECTION);

            const auto result = namespaceCollection.find_one(make_document(kvp("ern", ern)));
            return result.has_value();

        } catch (const std::exception &e) {
            log_error << "Namespace exists failed, ern: " << ern << ", error: " << e.what();
        }
        return false;
    }

    std::optional<Entity::EAM::Namespace> MongoEamRepository::findNamespaceByName(const std::string &accountId, const std::string &name) const {

        try {

            auto namespaceCollection = Database::instance().collection(NAMESPACE_COLLECTION);

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

            auto namespaceCollection = Database::instance().collection(NAMESPACE_COLLECTION);

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
            auto namespaceCollection = Database::instance().collection(NAMESPACE_COLLECTION);

            return static_cast<long>(namespaceCollection.count_documents(make_document(kvp("accountId", accountId))));
        } catch (const std::exception &e) {
            log_error << "Count namespaces failed, accountId: " << accountId << ", error: " << e.what();
        }
        return -1;
    }

    std::vector<Entity::EAM::Namespace> MongoEamRepository::listNamespaces(const std::string &accountId, const std::string &prefix, const long pageSize, const long pageIndex, const std::string &sortColumn, const std::string &sortDirection) const {

        std::vector<Entity::EAM::Namespace> namespaces;
        try {

            auto filter = prefix.empty()
                    ? make_document(kvp("accountId", accountId))
                    : make_document(kvp("accountId", accountId), kvp("name", make_document(kvp("$regex", "^" + prefix))));

            mongocxx::options::find opts;
            if (!sortColumn.empty()) {
                opts.sort(make_document(kvp(sortColumn, sortDirection == "asc" ? 1 : -1)));
            }
            if (pageSize > 0) {
                opts.limit(pageSize);
                opts.skip(std::max<long>(pageIndex, 0) * pageSize);
            }

            auto namespaceCollection = Database::instance().collection(NAMESPACE_COLLECTION);

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
            auto namespaceCollection = Database::instance().collection(NAMESPACE_COLLECTION);

            const auto result = namespaceCollection.delete_many(make_document(kvp("accountId", accountId), kvp("name", name)));
            log_debug << "Namespace deleted, count: " << result->deleted_count();

        } catch (const std::exception &e) {
            log_error << "Delete namespace failed, accountId: " << accountId << ", name: " << name << ", error: " << e.what();
        }
    }

    // ── Roles and grants ────────────────────────────────────────────────────

    Entity::EAM::Role MongoEamRepository::upsertRole(Entity::EAM::Role &role) {

        try {

            // Account and name together: a role name is unique within an account, not across the
            // installation, so matching on the name alone would have one account's role overwrite
            // another's.
            const auto filter = make_document(kvp("accountId", role.accountId), kvp("name", role.name));
            const auto update = make_document(kvp("$set", role.toDocument()));

            mongocxx::options::find_one_and_update opts;
            opts.upsert(true);
            opts.return_document(mongocxx::options::return_document::k_after);

            auto roleCollection = Database::instance().collection(ROLE_COLLECTION);

            if (auto result = roleCollection.find_one_and_update(filter.view(), update.view(), opts)) {
                return Entity::EAM::Role::fromDocument(result->view());
            }
            throw std::runtime_error("upsert returned no document, name: " + role.name);

        } catch (const std::exception &e) {
            log_error << "Upsert role failed, accountId: " << role.accountId << ", name: " << role.name << ", error: " << e.what();
            throw;
        }
    }

    std::optional<Entity::EAM::Role> MongoEamRepository::findRoleByName(const std::string &accountId, const std::string &name) const {

        try {

            auto roleCollection = Database::instance().collection(ROLE_COLLECTION);

            if (const auto result = roleCollection.find_one(make_document(kvp("accountId", accountId), kvp("name", name)))) {
                return Entity::EAM::Role::fromDocument(result->view());
            }

        } catch (const std::exception &e) {
            log_error << "Find role failed, accountId: " << accountId << ", name: " << name << ", error: " << e.what();
        }
        return {};
    }

    long MongoEamRepository::countRoles(const std::string &accountId) const {

        try {
            auto roleCollection = Database::instance().collection(ROLE_COLLECTION);

            return static_cast<long>(roleCollection.count_documents(make_document(kvp("accountId", accountId))));

        } catch (const std::exception &e) {
            log_error << "Count roles failed, accountId: " << accountId << ", error: " << e.what();
        }
        return -1;
    }

    std::vector<Entity::EAM::Role> MongoEamRepository::listRoles(const std::string &accountId, const std::string &prefix, const long pageSize,
                                                                 const long pageIndex, const std::string &sortColumn,
                                                                 const std::string &sortDirection) const {

        std::vector<Entity::EAM::Role> roles;
        try {

            const auto filter = prefix.empty()
                                        ? make_document(kvp("accountId", accountId))
                                        : make_document(kvp("accountId", accountId), kvp("name", make_document(kvp("$regex", "^" + prefix))));

            mongocxx::options::find opts;
            if (!sortColumn.empty()) {
                opts.sort(make_document(kvp(sortColumn, sortDirection == "asc" ? 1 : -1)));
            }
            if (pageSize > 0) {
                opts.limit(pageSize);
                opts.skip(std::max<long>(pageIndex, 0) * pageSize);
            }

            auto roleCollection = Database::instance().collection(ROLE_COLLECTION);

            for (auto cursor = roleCollection.find(filter.view(), opts); auto doc: cursor) {
                roles.push_back(Entity::EAM::Role::fromDocument(doc));
            }

        } catch (const std::exception &e) {
            log_error << "List roles failed, accountId: " << accountId << ", error: " << e.what();
        }
        return roles;
    }

    void MongoEamRepository::deleteRole(const std::string &accountId, const std::string &name) const {

        try {
            auto roleCollection = Database::instance().collection(ROLE_COLLECTION);

            const auto result = roleCollection.delete_many(make_document(kvp("accountId", accountId), kvp("name", name)));
            log_debug << "Role deleted, accountId: " << accountId << ", name: " << name << ", count: " << result->deleted_count();

        } catch (const std::exception &e) {
            log_error << "Delete role failed, accountId: " << accountId << ", name: " << name << ", error: " << e.what();
        }
    }

    Entity::EAM::Grant MongoEamRepository::addGrant(Entity::EAM::Grant &grant) {

        try {

            auto grantCollection = Database::instance().collection(GRANT_COLLECTION);

            // Inserted, not upserted: the same role granted to the same principal in two namespaces
            // is two grants, and collapsing them would silently drop one of the two scopes.
            const auto oid = grantCollection.insert_one(grant.toDocument().view());
            if (oid.has_value()) grant.oid = oid->to_string();

            log_debug << "Grant added, role: " << grant.role << ", principal: " << grant.principal;
            return grant;

        } catch (const std::exception &e) {
            log_error << "Add grant failed, role: " << grant.role << ", principal: " << grant.principal << ", error: " << e.what();
            throw;
        }
    }

    std::vector<Entity::EAM::Grant> MongoEamRepository::findGrantsByPrincipals(const std::vector<std::string> &principals) const {

        std::vector<Entity::EAM::Grant> grants;

        // No principals is not "every grant" - it is a caller who is nobody, and the honest answer
        // is nothing rather than everything.
        if (principals.empty()) return grants;

        try {

            bsoncxx::builder::basic::array principalArray;
            for (const auto &principal: principals) principalArray.append(principal);

            const auto filter = make_document(kvp("principal", make_document(kvp("$in", principalArray))));

            auto grantCollection = Database::instance().collection(GRANT_COLLECTION);

            for (auto cursor = grantCollection.find(filter.view()); auto doc: cursor) {
                grants.push_back(Entity::EAM::Grant::fromDocument(doc));
            }

        } catch (const std::exception &e) {
            log_error << "Find grants failed, principals: " << principals.size() << ", error: " << e.what();
        }
        return grants;
    }

    std::vector<Entity::EAM::Grant> MongoEamRepository::findGrantsByRole(const std::string &accountId, const std::string &role) const {

        std::vector<Entity::EAM::Grant> grants;
        try {

            auto grantCollection = Database::instance().collection(GRANT_COLLECTION);

            for (auto cursor = grantCollection.find(make_document(kvp("accountId", accountId), kvp("role", role))); auto doc: cursor) {
                grants.push_back(Entity::EAM::Grant::fromDocument(doc));
            }

        } catch (const std::exception &e) {
            log_error << "Find grants by role failed, accountId: " << accountId << ", role: " << role << ", error: " << e.what();
        }
        return grants;
    }

    namespace {

        // The filter behind both listGrants() and countGrants(), so that a page and the total it
        // is reported with cannot describe different sets. Which of the three it builds is decided
        // by which argument is non-empty - and a principal query is deliberately not account
        // scoped, because a principal ERN names one holder wherever their grants apply, which is
        // the question "what may they do" as it was asked before paging existed.
        bsoncxx::document::value grantFilter(const std::string &principal, const std::string &role, const std::string &accountId) {
            if (!principal.empty()) return make_document(kvp("principal", principal));
            if (!role.empty()) return make_document(kvp("accountId", accountId), kvp("role", role));
            return make_document(kvp("accountId", accountId));
        }

    }// namespace

    std::vector<Entity::EAM::Grant> MongoEamRepository::listGrants(const std::string &principal, const std::string &role, const std::string &accountId, const long pageSize, const long pageIndex, const std::string &sortColumn, const std::string &sortDirection) const {

        std::vector<Entity::EAM::Grant> grants;
        try {

            const auto filter = grantFilter(principal, role, accountId);

            mongocxx::options::find opts;
            if (!sortColumn.empty()) {
                opts.sort(make_document(kvp(sortColumn, sortDirection == "asc" ? 1 : -1)));
            }
            if (pageSize > 0) {
                opts.limit(pageSize);
                opts.skip(std::max<long>(pageIndex, 0) * pageSize);
            }

            auto grantCollection = Database::instance().collection(GRANT_COLLECTION);

            for (auto cursor = grantCollection.find(filter.view(), opts); auto doc: cursor) {
                grants.push_back(Entity::EAM::Grant::fromDocument(doc));
            }

        } catch (const std::exception &e) {
            log_error << "List grants failed, accountId: " << accountId << ", error: " << e.what();
        }
        return grants;
    }

    long MongoEamRepository::countGrants(const std::string &principal, const std::string &role, const std::string &accountId) const {

        try {
            const auto filter = grantFilter(principal, role, accountId);

            auto grantCollection = Database::instance().collection(GRANT_COLLECTION);

            return static_cast<long>(grantCollection.count_documents(filter.view()));
        } catch (const std::exception &e) {
            log_error << "Count grants failed, accountId: " << accountId << ", error: " << e.what();
        }
        return -1;
    }

    void MongoEamRepository::deleteGrant(const std::string &oid) const {

        try {
            auto grantCollection = Database::instance().collection(GRANT_COLLECTION);

            const auto result = grantCollection.delete_one(make_document(kvp("_id", bsoncxx::oid(oid))));
            log_debug << "Grant deleted, oid: " << oid << ", count: " << (result.has_value() ? result->deleted_count() : 0);

        } catch (const std::exception &e) {
            log_error << "Delete grant failed, oid: " << oid << ", error: " << e.what();
        }
    }

    void MongoEamRepository::deleteGrantsByPrincipal(const std::string &principal) const {

        try {
            auto grantCollection = Database::instance().collection(GRANT_COLLECTION);

            const auto result = grantCollection.delete_many(make_document(kvp("principal", principal)));
            log_debug << "Grants deleted, principal: " << principal << ", count: " << result->deleted_count();

        } catch (const std::exception &e) {
            log_error << "Delete grants failed, principal: " << principal << ", error: " << e.what();
        }
    }

}// namespace Euclid::Database