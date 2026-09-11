//
// Created by vogje01 on 9/10/26.
//

#pragma once

// C++ includes
#include <optional>
#include <string>
#include <vector>

// Euclid includes
#include <euclid/database/entity/ekv/Item.h>
#include <euclid/database/entity/ekv/SortCondition.h>
#include <euclid/database/entity/ekv/Table.h>

namespace Euclid::Database {

    /**
     * @brief Interface for the key/value store's tables and items.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class IEkvRepository {

    public:

        virtual ~IEkvRepository() = default;

        /**
         * @brief Creates a table.
         *
         * @param table the table to create.
         * @return the stored table.
         */
        virtual Entity::EKV::Table createTable(Entity::EKV::Table &table) = 0;

        /**
         * @brief Whether an account's namespace has a table of this name.
         *
         * @par
         * A table name is unique only within (accountId, nameSpace) - the same three fields the
         * unique index is built on, and the same three its ERN is built from - so all three are
         * needed to name one table. Throughout this interface an empty nameSpace means the
         * account's unscoped tables, not "any namespace".
         */
        [[nodiscard]]
        virtual bool tableExists(const std::string &accountId, const std::string &nameSpace, const std::string &name) const = 0;

        /**
         * @brief Finds a table by name within an account and namespace.
         */
        [[nodiscard]]
        virtual std::optional<Entity::EKV::Table> findTable(const std::string &accountId, const std::string &nameSpace, const std::string &name) const = 0;

        /**
         * @brief Lists the tables of one namespace of an account.
         *
         * @param accountId account whose tables to list.
         * @param nameSpace namespace within accountId whose tables to list.
         * @param prefix only tables whose name starts with this; empty matches all.
         * @param pageSize most to return; 0 or less means no limit.
         * @param pageIndex zero-based page, applied when pageSize is set.
         * @param sortColumn field to sort by; empty sorts by name.
         * @param sortDirection "asc" or "desc".
         */
        [[nodiscard]]
        virtual std::vector<Entity::EKV::Table> listTables(const std::string &accountId, const std::string &nameSpace,
                                                           const std::string &prefix, long pageSize,
                                                           long pageIndex, const std::string &sortColumn,
                                                           const std::string &sortDirection = "asc") const = 0;

        /**
         * @brief How many tables an account has in one namespace.
         */
        [[nodiscard]]
        virtual long countTables(const std::string &accountId, const std::string &nameSpace) const = 0;

        /**
         * @brief Deletes a table and everything in it.
         *
         * @return how many items went with it.
         */
        virtual long deleteTable(const std::string &accountId, const std::string &nameSpace, const std::string &name) = 0;

        /**
         * @brief Writes an item, replacing whatever was stored under its key.
         *
         * @param item the item, with its key already taken from its attributes and its namespace
         * already copied from its table by Entity::EKV::Item::FromAttributes().
         * @return the stored item.
         */
        virtual Entity::EKV::Item putItem(Entity::EKV::Item &item) = 0;

        /**
         * @brief Reads one item by its key.
         *
         * @param sortKey the sort key value, or std::nullopt for a table without one.
         */
        [[nodiscard]]
        virtual std::optional<Entity::EKV::Item> getItem(const std::string &accountId, const std::string &nameSpace,
                                                         const std::string &tableName,
                                                         const Entity::EKV::Value &partitionKey,
                                                         const std::optional<Entity::EKV::Value> &sortKey) const = 0;

        /**
         * @brief Removes one item by its key.
         *
         * @return true if there was one to remove.
         */
        virtual bool deleteItem(const std::string &accountId, const std::string &nameSpace,
                                const std::string &tableName,
                                const Entity::EKV::Value &partitionKey,
                                const std::optional<Entity::EKV::Value> &sortKey) = 0;

        /**
         * @brief Reads the items of one partition, in sort-key order.
         *
         * @param condition which of them to take; see Entity::EKV::SortCondition.
         * @param forward whether to read in ascending sort-key order.
         * @param pageSize most to return; 0 or less means no limit.
         * @param pageIndex zero-based page, applied when pageSize is set.
         */
        [[nodiscard]]
        virtual std::vector<Entity::EKV::Item> query(const std::string &accountId, const std::string &nameSpace,
                                                     const std::string &tableName,
                                                     const Entity::EKV::Value &partitionKey,
                                                     const Entity::EKV::SortCondition &condition, bool forward,
                                                     long pageSize, long pageIndex) const = 0;

        /**
         * @brief Reads a table's items without regard to their key.
         *
         * @par
         * Every item, a page at a time, in key order. What it costs is what it costs - a scan is a
         * scan - and it is here because a store nobody can enumerate is a store nobody can back
         * up, migrate or debug.
         */
        [[nodiscard]]
        virtual std::vector<Entity::EKV::Item> scan(const std::string &accountId, const std::string &nameSpace,
                                                    const std::string &tableName,
                                                    long pageSize, long pageIndex) const = 0;

        /**
         * @brief How many items a table holds.
         */
        [[nodiscard]]
        virtual long countItems(const std::string &accountId, const std::string &nameSpace, const std::string &tableName) const = 0;
    };

}// namespace Euclid::Database
