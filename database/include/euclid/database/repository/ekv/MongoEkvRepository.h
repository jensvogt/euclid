// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/10/26.
//

#pragma once

// Euclid includes
#include <euclid/core/LogStream.h>
#include <euclid/database/Database.h>
#include <euclid/database/repository/ekv/IEkvRepository.h>

namespace Euclid::Database {

    using namespace bsoncxx::builder::basic;

    /**
     * @brief EKV MongoDB database.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class MongoEkvRepository final : public IEkvRepository {

    public:

        /**
         * @brief Constructor
         */
        explicit MongoEkvRepository();

        /**
         * @brief Singleton instance
         */
        static MongoEkvRepository &instance() {
            static MongoEkvRepository repository;
            return repository;
        }

        Entity::EKV::Table createTable(Entity::EKV::Table &table) override;

        [[nodiscard]]
        bool tableExists(const std::string &accountId, const std::string &nameSpace, const std::string &name) const override;

        [[nodiscard]]
        std::optional<Entity::EKV::Table> findTable(const std::string &accountId, const std::string &nameSpace, const std::string &name) const override;

        [[nodiscard]]
        std::vector<Entity::EKV::Table> listTables(const std::string &accountId, const std::string &nameSpace,
                                                   const std::string &prefix, long pageSize,
                                                   long pageIndex, const std::string &sortColumn,
                                                   const std::string &sortDirection) const override;

        [[nodiscard]]
        long countTables(const std::string &accountId, const std::string &nameSpace) const override;

        long deleteTable(const std::string &accountId, const std::string &nameSpace, const std::string &name) override;

        Entity::EKV::Item putItem(Entity::EKV::Item &item) override;

        [[nodiscard]]
        std::optional<Entity::EKV::Item> getItem(const std::string &accountId, const std::string &nameSpace,
                                                 const std::string &tableName,
                                                 const Entity::EKV::Value &partitionKey,
                                                 const std::optional<Entity::EKV::Value> &sortKey) const override;

        bool deleteItem(const std::string &accountId, const std::string &nameSpace,
                        const std::string &tableName,
                        const Entity::EKV::Value &partitionKey,
                        const std::optional<Entity::EKV::Value> &sortKey) override;

        [[nodiscard]]
        std::vector<Entity::EKV::Item> query(const std::string &accountId, const std::string &nameSpace,
                                             const std::string &tableName,
                                             const Entity::EKV::Value &partitionKey,
                                             const Entity::EKV::SortCondition &condition, bool forward,
                                             long pageSize, long pageIndex) const override;

        [[nodiscard]]
        std::vector<Entity::EKV::Item> scan(const std::string &accountId, const std::string &nameSpace,
                                            const std::string &tableName,
                                            long pageSize, long pageIndex) const override;

        [[nodiscard]]
        long countItems(const std::string &accountId, const std::string &nameSpace, const std::string &tableName) const override;

    private:

        /**
         * @brief Creates the indexes the store is unusable without: one table per name per
         * account and namespace, one item per key per table.
         */
        void ensureIndexes() const;

        /**
         * @brief The filter that picks exactly one table.
         */
        [[nodiscard]]
        static bsoncxx::document::value tableFilter(const std::string &accountId, const std::string &nameSpace,
                                                    const std::string &name);

        /**
         * @brief The filter that picks every item of one table, and nothing else.
         */
        [[nodiscard]]
        static bsoncxx::document::value scopeFilter(const std::string &accountId, const std::string &nameSpace,
                                                    const std::string &tableName);

        /**
         * @brief The filter that picks exactly one item out of a table.
         */
        [[nodiscard]]
        static bsoncxx::document::value keyFilter(const std::string &accountId, const std::string &nameSpace,
                                                  const std::string &tableName,
                                                  const Entity::EKV::Value &partitionKey,
                                                  const std::optional<Entity::EKV::Value> &sortKey);

        static constexpr auto TABLE_COLLECTION = "ekv_table";
        static constexpr auto ITEM_COLLECTION = "ekv_item";
    };

}// namespace Euclid::Database
