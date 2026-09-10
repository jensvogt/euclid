//
// Created by vogje01 on 9/10/26.
//

// C++ includes
#include <chrono>

// Euclid includes
#include <euclid/database/repository/ekv/MongoEkvRepository.h>

namespace Euclid::Database {

    namespace {

        // What sorts after every string sharing a prefix, so that "begins with" is a range the
        // index can answer rather than a pattern every item has to be tested against. U+10FFFF is
        // the last code point there is, so nothing a caller can store sorts above it.
        constexpr auto kAfterEveryString = "\xF4\x8F\xBF\xBF";

        long timestampMillis(const std::chrono::system_clock::time_point &when) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(when.time_since_epoch()).count();
        }

    }// namespace

    MongoEkvRepository::MongoEkvRepository() {
        ensureIndexes();
    }

    void MongoEkvRepository::ensureIndexes() const {

        try {
            const auto tables = Database::instance().collection(TABLE_COLLECTION);

            // A table name is unique within an account, the same way every other named resource is.
            mongocxx::options::index tableOpts;
            tableOpts.unique(true);
            tables.create_index(make_document(kvp("accountId", 1), kvp("name", 1)), tableOpts);

            const auto items = Database::instance().collection(ITEM_COLLECTION);

            // The index the store is built on. Unique, because a key identifies one item by
            // definition; compound in this order, because it answers all three things asked of it:
            // one item by its whole key, one partition's items in sort order, and one table's
            // items for a scan.
            mongocxx::options::index itemOpts;
            itemOpts.unique(true);
            items.create_index(make_document(kvp("accountId", 1), kvp("tableName", 1), kvp("pk", 1), kvp("sk", 1)), itemOpts);

        } catch (const std::exception &e) {
            log_error << "Ensure EKV indexes failed, error: " << e.what();
        }
    }

    // ── Tables ───────────────────────────────────────────────────────────────

    Entity::EKV::Table MongoEkvRepository::createTable(Entity::EKV::Table &table) {

        try {
            const auto collection = Database::instance().collection(TABLE_COLLECTION);
            const auto result = collection.insert_one(table.toDocument().view());
            if (!result) throw std::runtime_error("insert returned no result, table: " + table.name);

            return findTable(table.accountId, table.name).value_or(table);

        } catch (const std::exception &e) {
            log_error << "Create table failed, table: " << table.name << ", error: " << e.what();
            throw;
        }
    }

    bool MongoEkvRepository::tableExists(const std::string &accountId, const std::string &name) const {

        try {
            const auto collection = Database::instance().collection(TABLE_COLLECTION);
            return collection.find_one(make_document(kvp("accountId", accountId), kvp("name", name))).has_value();

        } catch (const std::exception &e) {
            log_error << "Table exists failed, table: " << name << ", error: " << e.what();
        }
        return false;
    }

    std::optional<Entity::EKV::Table> MongoEkvRepository::findTable(const std::string &accountId, const std::string &name) const {

        try {
            const auto collection = Database::instance().collection(TABLE_COLLECTION);
            if (auto result = collection.find_one(make_document(kvp("accountId", accountId), kvp("name", name)))) {
                return Entity::EKV::Table::fromDocument(result.value());
            }

        } catch (const std::exception &e) {
            log_error << "Find table failed, table: " << name << ", error: " << e.what();
        }
        return {};
    }

    std::vector<Entity::EKV::Table> MongoEkvRepository::listTables(const std::string &accountId, const std::string &prefix, const long pageSize,
                                                                   const long pageIndex, const std::string &sortColumn,
                                                                   const std::string &sortDirection) const {

        std::vector<Entity::EKV::Table> tables;
        try {
            document filter;
            filter.append(kvp("accountId", accountId));
            if (!prefix.empty()) {
                filter.append(kvp("name", make_document(kvp("$gte", prefix), kvp("$lt", prefix + kAfterEveryString))));
            }

            mongocxx::options::find opts;
            opts.sort(make_document(kvp(sortColumn.empty() ? "name" : sortColumn, sortDirection == "desc" ? -1 : 1)));
            if (pageSize > 0) {
                opts.limit(pageSize);
                opts.skip(pageIndex * pageSize);
            }

            const auto collection = Database::instance().collection(TABLE_COLLECTION);
            for (auto cursor = collection.find(filter.extract().view(), opts); const auto &document: cursor) {
                tables.push_back(Entity::EKV::Table::fromDocument(document));
            }

        } catch (const std::exception &e) {
            log_error << "List tables failed, accountId: " << accountId << ", error: " << e.what();
        }
        return tables;
    }

    long MongoEkvRepository::countTables(const std::string &accountId) const {

        try {
            const auto collection = Database::instance().collection(TABLE_COLLECTION);
            return static_cast<long>(collection.count_documents(make_document(kvp("accountId", accountId))));

        } catch (const std::exception &e) {
            log_error << "Count tables failed, accountId: " << accountId << ", error: " << e.what();
        }
        return 0;
    }

    long MongoEkvRepository::deleteTable(const std::string &accountId, const std::string &name) {

        try {
            // The items first. A table whose row is gone but whose items are not would be invisible
            // and undeletable; items without a table are merely orphaned, and the next attempt
            // clears them.
            const auto items = Database::instance().collection(ITEM_COLLECTION);
            const auto removed = items.delete_many(make_document(kvp("accountId", accountId), kvp("tableName", name)));

            const auto tables = Database::instance().collection(TABLE_COLLECTION);
            tables.delete_one(make_document(kvp("accountId", accountId), kvp("name", name)));

            return removed ? static_cast<long>(removed->deleted_count()) : 0;

        } catch (const std::exception &e) {
            log_error << "Delete table failed, table: " << name << ", error: " << e.what();
            throw;
        }
    }

    // ── Items ────────────────────────────────────────────────────────────────

    bsoncxx::document::value MongoEkvRepository::keyFilter(const std::string &accountId, const std::string &tableName,
                                                           const Entity::EKV::Value &partitionKey,
                                                           const std::optional<Entity::EKV::Value> &sortKey) {

        document filter;
        filter.append(kvp("accountId", accountId), kvp("tableName", tableName));
        partitionKey.AppendTo(filter, "pk");

        // Absent rather than null when the table has no sort key, so this matches the document
        // exactly as it was written - and so a table with a sort key cannot be read as one without.
        if (sortKey.has_value()) sortKey->AppendTo(filter, "sk");
        else filter.append(kvp("sk", make_document(kvp("$exists", false))));

        return filter.extract();
    }

    Entity::EKV::Item MongoEkvRepository::putItem(Entity::EKV::Item &item) {

        try {
            const auto filter = keyFilter(item.accountId, item.tableName, item.partitionKey, item.sortKey);

            const auto update = make_document(
                    kvp("$set", item.toDocument()),
                    kvp("$setOnInsert", make_document(kvp("created", bsoncxx::types::b_date{
                                                                             std::chrono::milliseconds{timestampMillis(item.created)}}))),
                    kvp("$currentDate", make_document(kvp("modified", true))));

            mongocxx::options::find_one_and_update opts;
            opts.upsert(true);
            opts.return_document(mongocxx::options::return_document::k_after);

            const auto collection = Database::instance().collection(ITEM_COLLECTION);
            if (auto result = collection.find_one_and_update(filter.view(), update.view(), opts)) {
                return Entity::EKV::Item::fromDocument(result->view());
            }
            throw std::runtime_error("upsert returned no document, table: " + item.tableName);

        } catch (const std::exception &e) {
            log_error << "Put item failed, table: " << item.tableName << ", error: " << e.what();
            throw;
        }
    }

    std::optional<Entity::EKV::Item> MongoEkvRepository::getItem(const std::string &accountId, const std::string &tableName,
                                                                 const Entity::EKV::Value &partitionKey,
                                                                 const std::optional<Entity::EKV::Value> &sortKey) const {

        try {
            const auto collection = Database::instance().collection(ITEM_COLLECTION);
            if (auto result = collection.find_one(keyFilter(accountId, tableName, partitionKey, sortKey).view())) {
                return Entity::EKV::Item::fromDocument(result.value());
            }

        } catch (const std::exception &e) {
            log_error << "Get item failed, table: " << tableName << ", error: " << e.what();
        }
        return {};
    }

    bool MongoEkvRepository::deleteItem(const std::string &accountId, const std::string &tableName,
                                        const Entity::EKV::Value &partitionKey,
                                        const std::optional<Entity::EKV::Value> &sortKey) {

        try {
            const auto collection = Database::instance().collection(ITEM_COLLECTION);
            const auto result = collection.delete_one(keyFilter(accountId, tableName, partitionKey, sortKey).view());
            return result && result->deleted_count() > 0;

        } catch (const std::exception &e) {
            log_error << "Delete item failed, table: " << tableName << ", error: " << e.what();
            throw;
        }
    }

    std::vector<Entity::EKV::Item> MongoEkvRepository::query(const std::string &accountId, const std::string &tableName,
                                                             const Entity::EKV::Value &partitionKey,
                                                             const Entity::EKV::SortCondition &condition, const bool forward,
                                                             const long pageSize, const long pageIndex) const {

        std::vector<Entity::EKV::Item> items;
        try {
            document filter;
            filter.append(kvp("accountId", accountId), kvp("tableName", tableName));
            partitionKey.AppendTo(filter, "pk");

            // Every one of these is a bound on the indexed sort key, which is what lets the
            // database walk the range rather than read the partition and discard most of it.
            using Operator = Entity::EKV::SortCondition::Operator;
            switch (condition.op) {

                case Operator::Equals:
                    condition.value.AppendTo(filter, "sk");
                    break;

                case Operator::LessThan:
                case Operator::LessOrEqual:
                case Operator::GreaterThan:
                case Operator::GreaterOrEqual: {
                    const auto *comparison = condition.op == Operator::LessThan      ? "$lt"
                                             : condition.op == Operator::LessOrEqual ? "$lte"
                                             : condition.op == Operator::GreaterThan ? "$gt"
                                                                                     : "$gte";
                    document bound;
                    condition.value.AppendTo(bound, comparison);
                    filter.append(kvp("sk", bound.extract()));
                    break;
                }

                case Operator::Between: {
                    document bounds;
                    condition.value.AppendTo(bounds, "$gte");
                    condition.upper.AppendTo(bounds, "$lte");
                    filter.append(kvp("sk", bounds.extract()));
                    break;
                }

                case Operator::BeginsWith: {
                    // A range rather than a pattern: no regular expression to escape, and the index
                    // is used. Strings only, which is checked before this is called.
                    const auto prefix = condition.value.holds<std::string>() ? condition.value.get<std::string>() : std::string{};
                    filter.append(kvp("sk", make_document(kvp("$gte", prefix), kvp("$lt", prefix + kAfterEveryString))));
                    break;
                }

                case Operator::None:
                default:
                    break;
            }

            mongocxx::options::find opts;
            opts.sort(make_document(kvp("sk", forward ? 1 : -1)));
            if (pageSize > 0) {
                opts.limit(pageSize);
                opts.skip(pageIndex * pageSize);
            }

            const auto collection = Database::instance().collection(ITEM_COLLECTION);
            for (auto cursor = collection.find(filter.extract().view(), opts); const auto &document: cursor) {
                items.push_back(Entity::EKV::Item::fromDocument(document));
            }

        } catch (const std::exception &e) {
            log_error << "Query failed, table: " << tableName << ", error: " << e.what();
            throw;
        }
        return items;
    }

    std::vector<Entity::EKV::Item> MongoEkvRepository::scan(const std::string &accountId, const std::string &tableName,
                                                            const long pageSize, const long pageIndex) const {

        std::vector<Entity::EKV::Item> items;
        try {
            mongocxx::options::find opts;

            // In key order, so that paging through a scan sees each item once: an unordered scan
            // paged with skip and limit can miss items and repeat others as the collection changes
            // underneath it.
            opts.sort(make_document(kvp("pk", 1), kvp("sk", 1)));
            if (pageSize > 0) {
                opts.limit(pageSize);
                opts.skip(pageIndex * pageSize);
            }

            const auto collection = Database::instance().collection(ITEM_COLLECTION);
            const auto filter = make_document(kvp("accountId", accountId), kvp("tableName", tableName));
            for (auto cursor = collection.find(filter.view(), opts); const auto &document: cursor) {
                items.push_back(Entity::EKV::Item::fromDocument(document));
            }

        } catch (const std::exception &e) {
            log_error << "Scan failed, table: " << tableName << ", error: " << e.what();
            throw;
        }
        return items;
    }

    long MongoEkvRepository::countItems(const std::string &accountId, const std::string &tableName) const {

        try {
            const auto collection = Database::instance().collection(ITEM_COLLECTION);
            return static_cast<long>(collection.count_documents(make_document(kvp("accountId", accountId), kvp("tableName", tableName))));

        } catch (const std::exception &e) {
            log_error << "Count items failed, table: " << tableName << ", error: " << e.what();
        }
        return 0;
    }

}// namespace Euclid::Database
