//
// Created by vogje01 on 9/10/26.
//

#pragma once

// C++ includes
#include <chrono>
#include <optional>
#include <string>

// Euclid includes
#include <euclid/database/entity/ekv/Table.h>
#include <euclid/database/entity/ekv/Value.h>

namespace Euclid::Database::Entity::EKV {

    /**
     * @brief One stored item: its key, and everything it consists of.
     *
     * @par
     * The key attributes are held twice - once in @p attributes, where the caller put them and
     * where they are read back from, and once as @p partitionKey / @p sortKey, which are what the
     * index is built on. The second copy is what makes a lookup a lookup rather than a scan, and
     * keeping the first is what makes an item come back as it was written.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    struct Item {

        /**
         * @brief ID
         */
        std::string oid;

        /**
         * @brief Table the item belongs to.
         */
        std::string tableName;

        /**
         * @brief Account the table belongs to.
         */
        std::string accountId;

        /**
         * @brief Namespace the table belongs to, or empty for an unscoped one.
         *
         * @par
         * Copied from the table rather than taken from the request: it is part of what identifies
         * the item, and two namespaces of one account may each have a "suppliers" table whose
         * items are nothing to do with each other. Serialized as "namespace", like every other
         * entity's.
         */
        std::string nameSpace;

        /**
         * @brief Region the item was written in.
         */
        std::string region;

        /**
         * @brief The partition key's value, as taken from the attributes.
         */
        Value partitionKey;

        /**
         * @brief The sort key's value, where the table has one.
         */
        std::optional<Value> sortKey;

        /**
         * @brief Everything the item consists of, the key attributes included.
         */
        Map attributes;

        /**
         * @brief Creation timestamp - of the item, not of this version of it.
         */
        std::chrono::system_clock::time_point created;

        /**
         * @brief When it was last written.
         */
        std::chrono::system_clock::time_point modified;

        /**
         * @brief Converts the entity to a MongoDB document.
         */
        [[nodiscard]]
        bsoncxx::document::value toDocument() const;

        /**
         * @brief Converts a MongoDB document to an entity.
         */
        static Item fromDocument(const std::optional<bsoncxx::document::view> &document);

        /**
         * @brief Takes the key values out of an item's attributes, checking them against the
         * table's key schema.
         *
         * @par
         * Where a key is settled: an item that does not carry the attributes its table is keyed
         * on, or carries them with the wrong type, is not a member of that table and is refused
         * here rather than stored somewhere it can never be found.
         *
         * @param table the table the item is destined for.
         * @param attributes the item.
         * @param error set to what is wrong, untouched when nothing is.
         * @return the item with its key filled in, or std::nullopt.
         */
        static std::optional<Item> FromAttributes(const Table &table, const Map &attributes, std::string &error);

        /**
         * @brief Whether a value matches what a key schema calls for.
         */
        static bool Matches(const KeySchema &schema, const Value &value);
    };

}// namespace Euclid::Database::Entity::EKV
