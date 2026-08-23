//
// Created by vogje01 on 8/23/26.
//

#pragma once

// C++ includes
#include <chrono>
#include <optional>
#include <string>

// MongoDB includes
#include <bsoncxx/document/value-fwd.hpp>
#include <bsoncxx/builder/basic/document.hpp>

namespace Euclid::Database::Entity::Storage {

    using std::chrono::system_clock;

    /**
     * @brief Storage object entity.
     *
     * Represents one bucket object. The object's bytes live in a flat file on disk named after
     * internalName (a UUID); key is only ever resolved to that filename through this entity, so
     * the database is the sole source of truth for the key-to-file mapping.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    struct Object final {

        /**
         * @brief Region
         */
        std::string region;

        /**
         * @brief Owner
         */
        std::string owner;

        /**
         * @brief ID
         */
        std::string oid;

        /**
         * @brief ERN of the bucket this object belongs to
         */
        std::string bucketErn;

        /**
         * @brief Object key (path) within the bucket
         */
        std::string key;

        /**
         * @brief Internal name (UUID) of the flat file this object's bytes are stored under
         */
        std::string internalName;

        /**
         * @brief Object ERN
         */
        std::string ern;

        /**
         * @brief Object size in bytes
         */
        long size = 0;

        /**
         * @brief Creation date
         */
        system_clock::time_point created = system_clock::now();

        /**
         * @brief Last modification date
         */
        system_clock::time_point modified = system_clock::now();

        /**
         * @brief Converts the entity to a MongoDB document
         *
         * @return entity as a MongoDB document.
         */
        [[nodiscard]]
        bsoncxx::document::value toDocument() const;

        /**
         * @brief Converts the MongoDB document to an entity
         *
         * @param document MongoDB document.
         */
        static Object fromDocument(const std::optional<bsoncxx::document::view> &document);
    };

}// namespace Euclid::Database::Entity::Storage
