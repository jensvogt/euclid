//
// Created by vogje01 on 01/06/2023.
//

#pragma once

// C++ includes
#include <chrono>
#include <optional>
#include <string>

// MongoDB includes
#include <bsoncxx/document/value-fwd.hpp>
#include <bsoncxx/builder/basic/document.hpp>

// Euclid includes

namespace Euclid::Database::Entity::Storage {

    using std::chrono::system_clock;

    /**
     * @brief Storage bucket entity
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    struct Bucket final {

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
         * @brief Bucket name
         */
        std::string name;

        /**
         * @brief Bucket ERN
         */
        std::string ern;

        /**
         * @brief Bucket size in bytes
         */
        long size{};

        /**
         * @brief Number of objects
         */
        long objects{};

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
        static Bucket fromDocument(const std::optional<bsoncxx::document::view> &document);
    };

}// namespace Euclid::Database::Entity::SQS