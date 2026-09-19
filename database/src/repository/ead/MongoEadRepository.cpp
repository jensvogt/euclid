// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <vector>
#include <algorithm>
#include <tuple>

// MongoDB includes
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/index.hpp>

// Euclid includes
#include <euclid/database/repository/ead/MongoEadRepository.h>

namespace Euclid::Database {

    namespace {

        // The filter every read here shares. Each field is optional and left out when empty, so
        // "no filter" really is no filter rather than a match on empty strings.
        bsoncxx::document::value filterOf(const std::string &accountId, const std::string &userId,
                                          const std::string &moduleName, const std::string &command) {

            bsoncxx::builder::basic::document filter;
            if (!accountId.empty()) filter.append(kvp("accountId", accountId));
            if (!userId.empty()) filter.append(kvp("userId", userId));
            if (!moduleName.empty()) filter.append(kvp("moduleName", moduleName));
            if (!command.empty()) filter.append(kvp("command", command));
            return filter.extract();
        }

    }// namespace

    MongoEadRepository::MongoEadRepository() {

        try {
            auto collection = Database::instance().collection(COLLECTION);

            // What every listing sorts by, and the one index a trail genuinely needs: the question
            // is always "what happened recently", optionally narrowed. Compound with accountId
            // first because narrowing by account is the common case and it is also the boundary
            // the reading side enforces.
            collection.create_index(make_document(kvp("accountId", 1), kvp("created", -1)));

            // The three narrowings the CLI offers. Each is paired with created so the sort comes
            // from the index rather than from a blocking sort over the matches - the mistake ENS
            // resend made, which cost hours on a large collection.
            collection.create_index(make_document(kvp("userId", 1), kvp("created", -1)));
            collection.create_index(make_document(kvp("moduleName", 1), kvp("created", -1)));
            collection.create_index(make_document(kvp("created", -1)));

            // Retention, enforced by the database. expireAfterSeconds is zero because the moment
            // is already in the document, the same shape ens_message and ees_events use.
            mongocxx::options::index expiresAtOpts;
            expiresAtOpts.expire_after(std::chrono::seconds(0));
            collection.create_index(make_document(kvp("expiresAt", 1)), expiresAtOpts);

        } catch (const std::exception &e) {
            log_error << "Ensure EAD indexes failed, error: " << e.what();
        }
    }

    long MongoEadRepository::createEvents(const std::vector<Entity::EAD::AuditEvent> &events) {

        if (events.empty()) return 0;

        try {
            std::vector<bsoncxx::document::value> documents;
            documents.reserve(events.size());
            for (const auto &event: events) documents.push_back(event.toDocument());

            return Database::instance().collection(COLLECTION).insert_many(documents);

        } catch (const std::exception &e) {
            // Logged and swallowed, exactly as the single write below. A module that cannot write
            // its trail must still answer its callers, and the batch is already off the queue -
            // there is nobody left to tell.
            log_error << "Create audit events failed, count: " << events.size() << ", error: " << e.what();
            return 0;
        }
    }

    Entity::EAD::AuditEvent MongoEadRepository::createEvent(const Entity::EAD::AuditEvent &event) {

        try {
            auto collection = Database::instance().collection(COLLECTION);
            std::ignore = collection.insert_one(event.toDocument().view());

        } catch (const std::exception &e) {
            // Logged and swallowed. An audit write that threw into the request that caused it
            // would make recording a command a way to fail it, and a module that cannot write its
            // trail must still answer its callers.
            log_error << "Create audit event failed, module: " << event.moduleName
                      << ", command: " << event.command << ", error: " << e.what();
        }
        return event;
    }

    std::vector<Entity::EAD::AuditEvent> MongoEadRepository::listEvents(const std::string &accountId, const std::string &userId,
                                                                       const std::string &moduleName, const std::string &command,
                                                                       const long pageSize, const long pageIndex) const {

        std::vector<Entity::EAD::AuditEvent> events;
        try {
            mongocxx::options::find opts;
            opts.sort(make_document(kvp("created", -1)));
            if (pageSize > 0) {
                opts.limit(pageSize);
                opts.skip(std::max<long>(pageIndex, 0) * pageSize);
            }

            auto collection = Database::instance().collection(COLLECTION);
            const auto filter = filterOf(accountId, userId, moduleName, command);

            for (auto cursor = collection.find(filter.view(), opts); auto doc: cursor) {
                events.push_back(Entity::EAD::AuditEvent::fromDocument(doc));
            }

        } catch (const std::exception &e) {
            log_error << "List audit events failed, error: " << e.what();
        }
        return events;
    }

    long MongoEadRepository::countEvents(const std::string &accountId, const std::string &userId,
                                         const std::string &moduleName, const std::string &command,
                                         const long limit) const {

        try {
            auto collection = Database::instance().collection(COLLECTION);
            const auto filter = filterOf(accountId, userId, moduleName, command);
            return static_cast<long>(collection.count_documents(filter.view(), limit));

        } catch (const std::exception &e) {
            log_error << "Count audit events failed, error: " << e.what();
            return 0;
        }
    }

    long MongoEadRepository::purgeEvents(const std::chrono::system_clock::time_point &before) {

        try {
            auto collection = Database::instance().collection(COLLECTION);
            const auto filter = make_document(kvp("created", make_document(kvp("$lt", bsoncxx::types::b_date(before)))));

            const auto result = collection.delete_many(filter.view());
            return result ? static_cast<long>(result->deleted_count()) : 0;

        } catch (const std::exception &e) {
            log_error << "Purge audit events failed, error: " << e.what();
            return 0;
        }
    }

}// namespace Euclid::Database
