//
// Created by vogje01 on 9/10/26.
//

#pragma once

// C++ includes
#include <chrono>
#include <optional>
#include <string>

// MongoDB includes
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view-fwd.hpp>

namespace Euclid::Database::Entity::EKV {

    /**
     * @brief What a key attribute may be.
     *
     * @par
     * Fewer types than a value can hold, deliberately: a key is compared and ordered, and these
     * are the ones an ordering means something for.
     */
    enum class KeyType {
        String,
        Number,
        Binary
    };

    /**
     * @brief Turns a key type into the word used for it in the API and the configuration.
     */
    std::string ToString(KeyType type);

    /**
     * @brief Reads a key type from that word.
     *
     * @param name "string", "number" or "binary", in any case.
     * @return the type, or std::nullopt if it is none of them.
     */
    std::optional<KeyType> KeyTypeFromString(const std::string &name);

    /**
     * @brief One half of a table's key: which attribute it is, and what type that attribute has.
     */
    struct KeySchema {

        /**
         * @brief The attribute this key is taken from.
         */
        std::string name;

        /**
         * @brief What type that attribute has to be, in every item.
         */
        KeyType type{KeyType::String};
    };

    /**
     * @brief A table: a name, a key, and the items stored under it.
     *
     * @par
     * The partition key alone identifies an item where there is no sort key; with one, the pair
     * does, and items sharing a partition key are ordered by their sort key - which is what makes
     * a range query over them possible, and the only reason a table would declare one.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    struct Table {

        /**
         * @brief ID
         */
        std::string oid;

        /**
         * @brief Table name, unique within an account.
         */
        std::string name;

        /**
         * @brief Euclid resource name.
         */
        std::string ern;

        /**
         * @brief Account the table belongs to.
         */
        std::string accountId;

        /**
         * @brief Region the table was created in.
         */
        std::string region;

        /**
         * @brief Namespace the table belongs to, or empty for an unscoped one.
         */
        std::string nameSpace;

        /**
         * @brief The attribute every item is identified by.
         */
        KeySchema partitionKey;

        /**
         * @brief The attribute items within a partition are ordered by, if the table has one.
         */
        std::optional<KeySchema> sortKey;

        /**
         * @brief Creation timestamp.
         */
        std::chrono::system_clock::time_point created;

        /**
         * @brief Modification timestamp.
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
        static Table fromDocument(const std::optional<bsoncxx::document::view> &document);
    };

}// namespace Euclid::Database::Entity::EKV
