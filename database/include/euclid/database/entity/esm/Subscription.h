//
// Created by vogje01 on 8/27/26.
//

#pragma once

#include <vector>

// C++ includes
#include <chrono>
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
     * @brief A subscription fans out object-created notifications from an ESM bucket (sourceErn)
     * to a target resource (targetErn) of the given protocol type. Only type "SQS" (an EQS queue
     * ERN as targetErn) is supported for now.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    struct Subscription final : BaseEntity {

        /**
         * @brief Owner
         */
        std::string owner;

        /**
         * @brief ID
         */
        std::string oid;

        /**
         * @brief Subscription ERN
         */
        std::string ern;

        /**
         * @brief ERN of the bucket objects are created in.
         */
        std::string sourceErn;

        /**
         * @brief Delivery protocol. Only "SQS" is supported for now.
         */
        std::string type;

        /**
         * @brief ERN of the delivery target. For type "SQS", an EQS queue ERN.
         */
        std::string targetErn;

        /**
         * @brief Which object events to deliver, empty for all of them.
         *
         * @par
         * The event bus names - "esm.object.created", "esm.object.updated", "esm.object.deleted" -
         * so a subscriber asks for the same things it would have asked the event service for.
         */
        std::vector<std::string> eventTypes;

        /**
         * @brief Only deliver objects whose key starts with this, empty for the whole bucket.
         */
        std::string prefix;

        /**
         * @brief Whether directory markers count as objects worth delivering.
         *
         * @par
         * A bucket is flat: a directory exists only as a zero-byte object whose key ends in "/".
         * Most subscribers want files and would have to discard those themselves, so they are left
         * out unless asked for.
         */
        bool directories = false;

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
        static Subscription fromDocument(const std::optional<bsoncxx::document::view> &document);
    };

}// namespace Euclid::Database::Entity::ESM
