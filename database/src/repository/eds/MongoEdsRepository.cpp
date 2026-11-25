// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// MongoDB includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/index.hpp>
#include <mongocxx/options/replace.hpp>

// Euclid includes
#include <euclid/database/repository/eds/MongoEdsRepository.h>

namespace Euclid::Database {

    using bsoncxx::builder::basic::document;
    using bsoncxx::builder::basic::kvp;
    using bsoncxx::builder::basic::make_document;

    namespace {

        // What sorts after every string sharing a prefix - see MongoEkvRepository.
        constexpr auto kAfterEveryString = "\xF4\x8F\xBF\xBF";

        void appendPrefix(document &filter, const std::string &field, const std::string &prefix) {
            if (prefix.empty()) return;
            filter.append(kvp(field, make_document(kvp("$gte", prefix), kvp("$lt", prefix + kAfterEveryString))));
        }

        document recordFilter(const std::string &zoneName, const std::string &name, const std::string &type) {
            document filter;
            filter.append(kvp("zoneName", zoneName));
            if (!name.empty()) filter.append(kvp("name", name));
            if (!type.empty()) filter.append(kvp("type", type));
            return filter;
        }

    }// namespace

    MongoEdsRepository::MongoEdsRepository() {
        // Deferred, as everywhere: on the EMD backend the store may not be listening yet.
        Database::instance().onReachable([this] { ensureIndexes(); });
    }

    void MongoEdsRepository::ensureIndexes() const {

        try {
            mongocxx::options::index unique;
            unique.unique(true);

            // By name alone: DNS has one namespace, so the installation has one zone of a name.
            Database::instance().collection(ZONE_COLLECTION).create_index(make_document(kvp("name", 1)), unique);
            Database::instance().collection(ZONE_COLLECTION).create_index(make_document(kvp("accountId", 1), kvp("namespace", 1), kvp("name", 1)));

            // A record set is one name and one type; this is also the order a zone is listed in.
            Database::instance().collection(RECORD_COLLECTION).create_index(make_document(kvp("zoneName", 1), kvp("name", 1), kvp("type", 1)), unique);

        } catch (const std::exception &e) {
            log_error << "Ensure EDS indexes failed, error: " << e.what();
        }
    }

    // ── Zones ────────────────────────────────────────────────────────────────

    Entity::EDS::HostedZone MongoEdsRepository::createZone(Entity::EDS::HostedZone &zone) {

        try {
            const auto collection = Database::instance().collection(ZONE_COLLECTION);
            if (const auto result = collection.insert_one(zone.toDocument().view()); !result) {
                throw std::runtime_error("insert returned no result, zone: " + zone.name);
            }
            return findZone(zone.name).value_or(zone);

        } catch (const std::exception &e) {
            log_error << "Create zone failed, zone: " << zone.name << ", error: " << e.what();
            throw;
        }
    }

    std::optional<Entity::EDS::HostedZone> MongoEdsRepository::findZone(const std::string &name) const {

        try {
            const auto collection = Database::instance().collection(ZONE_COLLECTION);
            if (const auto found = collection.find_one(make_document(kvp("name", name))); found.has_value()) {
                return Entity::EDS::HostedZone::fromDocument(found->view());
            }
        } catch (const std::exception &e) {
            log_error << "Find zone failed, zone: " << name << ", error: " << e.what();
            throw;
        }
        return std::nullopt;
    }

    std::vector<Entity::EDS::HostedZone> MongoEdsRepository::listZones(const std::string &accountId, const std::string &nameSpace,
                                                                       const std::string &prefix, const long pageSize, const long pageIndex) const {

        std::vector<Entity::EDS::HostedZone> zones;
        try {
            document filter;
            filter.append(kvp("accountId", accountId), kvp("namespace", nameSpace));
            appendPrefix(filter, "name", prefix);

            mongocxx::options::find opts;
            opts.sort(make_document(kvp("name", 1)));
            if (pageSize > 0) {
                opts.limit(pageSize);
                opts.skip(pageIndex * pageSize);
            }

            const auto collection = Database::instance().collection(ZONE_COLLECTION);
            for (auto cursor = collection.find(filter.extract().view(), opts); const auto &found: cursor) {
                zones.push_back(Entity::EDS::HostedZone::fromDocument(found));
            }
        } catch (const std::exception &e) {
            log_error << "List zones failed, accountId: " << accountId << ", error: " << e.what();
        }
        return zones;
    }

    long MongoEdsRepository::countZones(const std::string &accountId, const std::string &nameSpace, const std::string &prefix) const {

        try {
            document filter;
            filter.append(kvp("accountId", accountId), kvp("namespace", nameSpace));
            appendPrefix(filter, "name", prefix);
            return static_cast<long>(Database::instance().collection(ZONE_COLLECTION).count_documents(filter.extract().view()));

        } catch (const std::exception &e) {
            log_error << "Count zones failed, accountId: " << accountId << ", error: " << e.what();
        }
        return 0;
    }

    std::vector<Entity::EDS::HostedZone> MongoEdsRepository::listAllZones() const {

        std::vector<Entity::EDS::HostedZone> zones;
        // Not caught: the name server would rather keep serving the zones it has than replace
        // them with none because the database did not answer once.
        const auto collection = Database::instance().collection(ZONE_COLLECTION);
        for (auto cursor = collection.find(make_document().view()); const auto &found: cursor) {
            zones.push_back(Entity::EDS::HostedZone::fromDocument(found));
        }
        return zones;
    }

    long MongoEdsRepository::deleteZone(const std::string &name) {

        try {
            // The records first, as EKV deletes a table's items first: a zone row without records
            // is an empty zone, records without a zone row are invisible and undeletable.
            const auto records = Database::instance().collection(RECORD_COLLECTION).delete_many(make_document(kvp("zoneName", name)).view());
            Database::instance().collection(ZONE_COLLECTION).delete_one(make_document(kvp("name", name)).view());
            return records ? static_cast<long>(records->deleted_count()) : 0;

        } catch (const std::exception &e) {
            log_error << "Delete zone failed, zone: " << name << ", error: " << e.what();
            throw;
        }
    }

    bool MongoEdsRepository::updateZone(const Entity::EDS::HostedZone &zone) {

        try {
            const auto result = Database::instance().collection(ZONE_COLLECTION).update_one(
                    make_document(kvp("name", zone.name)),
                    make_document(kvp("$set", make_document(kvp("ttl", static_cast<std::int64_t>(zone.ttl)),
                                                            kvp("description", zone.description),
                                                            kvp("modified", bsoncxx::types::b_date{zone.modified})))));
            return result.has_value() && result->matched_count() > 0;

        } catch (const std::exception &e) {
            log_error << "Update zone failed, zone: " << zone.name << ", error: " << e.what();
            throw;
        }
    }

    void MongoEdsRepository::touchZone(const std::string &name, const std::chrono::system_clock::time_point when) {

        try {
            Database::instance().collection(ZONE_COLLECTION).update_one(
                    make_document(kvp("name", name)),
                    make_document(kvp("$set", make_document(kvp("modified", bsoncxx::types::b_date{when})))));

        } catch (const std::exception &e) {
            // Only the serial is late; the records themselves are written.
            log_warning << "Touch zone failed, zone: " << name << ", error: " << e.what();
        }
    }

    // ── Records ──────────────────────────────────────────────────────────────

    Entity::EDS::Record MongoEdsRepository::putRecord(Entity::EDS::Record &record) {

        try {
            // created survives a replacement: it is when the set first existed, not when it was
            // last written.
            if (const auto existing = findRecords(record.zoneName, record.name); !existing.empty()) {
                for (const auto &found: existing) {
                    if (found.type == record.type) record.created = found.created;
                }
            }

            mongocxx::options::replace options;
            options.upsert(true);
            Database::instance().collection(RECORD_COLLECTION).replace_one(
                    recordFilter(record.zoneName, record.name, record.type).extract(), record.toDocument().view(), options);
            return record;

        } catch (const std::exception &e) {
            log_error << "Put record failed, name: " << record.name << ", type: " << record.type << ", error: " << e.what();
            throw;
        }
    }

    std::vector<Entity::EDS::Record> MongoEdsRepository::findRecords(const std::string &zoneName, const std::string &name) const {
        return listRecords(zoneName, name, {}, 0, 0);
    }

    bool MongoEdsRepository::deleteRecord(const std::string &zoneName, const std::string &name, const std::string &type) {

        try {
            const auto result = Database::instance().collection(RECORD_COLLECTION).delete_one(recordFilter(zoneName, name, type).extract());
            return result.has_value() && result->deleted_count() > 0;

        } catch (const std::exception &e) {
            log_error << "Delete record failed, name: " << name << ", type: " << type << ", error: " << e.what();
            throw;
        }
    }

    std::vector<Entity::EDS::Record> MongoEdsRepository::listRecords(const std::string &zoneName, const std::string &name,
                                                                     const std::string &type, const long pageSize, const long pageIndex) const {

        std::vector<Entity::EDS::Record> records;
        try {
            mongocxx::options::find opts;
            opts.sort(make_document(kvp("name", 1), kvp("type", 1)));
            if (pageSize > 0) {
                opts.limit(pageSize);
                opts.skip(pageIndex * pageSize);
            }

            const auto collection = Database::instance().collection(RECORD_COLLECTION);
            for (auto cursor = collection.find(recordFilter(zoneName, name, type).extract(), opts); const auto &found: cursor) {
                records.push_back(Entity::EDS::Record::fromDocument(found));
            }
        } catch (const std::exception &e) {
            log_error << "List records failed, zone: " << zoneName << ", error: " << e.what();
        }
        return records;
    }

    long MongoEdsRepository::countRecords(const std::string &zoneName, const std::string &name, const std::string &type) const {

        try {
            return static_cast<long>(Database::instance().collection(RECORD_COLLECTION).count_documents(recordFilter(zoneName, name, type).extract()));
        } catch (const std::exception &e) {
            log_error << "Count records failed, zone: " << zoneName << ", error: " << e.what();
        }
        return 0;
    }

    std::vector<Entity::EDS::Record> MongoEdsRepository::listAllRecords() const {

        std::vector<Entity::EDS::Record> records;
        // Not caught, for the reason listAllZones() is not.
        const auto collection = Database::instance().collection(RECORD_COLLECTION);
        for (auto cursor = collection.find(make_document().view()); const auto &found: cursor) {
            records.push_back(Entity::EDS::Record::fromDocument(found));
        }
        return records;
    }

}// namespace Euclid::Database
