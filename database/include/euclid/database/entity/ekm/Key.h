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
#include <bsoncxx/document/value-fwd.hpp>
#include <bsoncxx/builder/basic/document.hpp>

// Euclid includes
#include <euclid/database/entity/BaseEntity.h>
#include <euclid/database/entity/ekm/KeyStatus.h>

namespace Euclid::Database::Entity::EKM {

    using std::chrono::system_clock;

    /**
     * @brief EKM key entity
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    struct Key final : BaseEntity {

        /**
         * @brief ID
         */
        std::string oid;

        /**
         * @brief Euclid resource name
         */
        std::string ern;

        /**
         * @brief Name
         */
        std::string name;

        /**
         * @brief What the key is for, in the words of whoever created it.
         *
         * @par
         * A key is named by a generated ID (see name), which says nothing about what it protects.
         * Since a key outlives the reason it was made - and deleting the wrong one is not a
         * recoverable mistake - this is where that reason is kept. Free text, never interpreted,
         * and empty for keys created before it existed or by a caller that gave none.
         */
        std::string description;

        /**
         * @brief Key algorithm name
         */
        std::string algorithm;

        /**
         * @brief Key length
         */
        long length = 128;

        /**
         * @brief Raw key material, base64-encoded. Never leaves the server - not part of any DTO.
         */
        std::string keyMaterial;

        /**
         * @brief Queue tags
         */
        std::map<std::string, std::string> tags;

        /**
         * @brief If set to anything other than the epoch, the point in time at which this key
         * will be permanently deleted by the purge sweep. Decryption stays available right up
         * until then - the whole point of the delay is to leave time to decrypt/migrate data
         * before it becomes unrecoverable - but see KeyStatus::PENDING_DELETION: encryption is
         * blocked as soon as a deletion is scheduled. Epoch (default) means the key is not
         * scheduled for deletion.
         */
        system_clock::time_point deletionDate{};

        /**
         * @brief Lifecycle status - see KeyStatus. Gates whether the key can still be used to
         * encrypt; decryption is never blocked by status, so already-encrypted data stays
         * recoverable regardless of status.
         */
        KeyStatus status = KeyStatus::AVAILABLE;

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
        static Key fromDocument(const std::optional<bsoncxx::document::view> &document);
    };

}// namespace Euclid::Database::Entity::SQS