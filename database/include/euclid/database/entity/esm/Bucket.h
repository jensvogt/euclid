//
// Created by vogje01 on 01/06/2023.
//

#pragma once

// C++ includes
#include <chrono>
#include <map>
#include <optional>
#include <string>

// MongoDB includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/document/value-fwd.hpp>

// Euclid includes
#include <euclid/database/entity/BaseEntity.h>

namespace Euclid::Database::Entity::ESM {

    using std::chrono::system_clock;

    /**
     * @brief Storage bucket entity
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    struct Bucket final : BaseEntity {

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
         * @brief Queue tags
         */
        std::map<std::string, std::string> tags;

        /**
         * @brief ERN of the EKM key objects written to this bucket are encrypted under, or empty
         * if the bucket stores objects in the clear.
         *
         * @par
         * A bucket-wide setting that applies from the moment it is made: objects already in the
         * bucket are left exactly as they were stored, and each object records the key it is
         * actually under (see Object::encryptionKeyErn), so a bucket can hold objects written
         * before encryption was enabled, after it, and under a previous key, all readable. This
         * field only ever answers "what happens to the next object written here".
         */
        std::string encryptionKeyErn;

        /**
         * @brief Bucket size in bytes
         */
        int64_t size{};

        /**
         * @brief Number of objects
         */
        int64_t objects{};

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