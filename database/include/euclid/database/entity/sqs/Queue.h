//
// Created by vogje01 on 01/06/2023.
//

#pragma once

// C++ includes
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

// MongoDB includes
#include <bsoncxx/document/value-fwd.hpp>
#include <bsoncxx/builder/basic/document.hpp>

// Euclid includes
#include <euclid/database/entity/sqs/QueueAttribute.h>

namespace Euclid::Database::Entity::SQS {

    using std::chrono::system_clock;

    /**
     * @brief SQS queue entity
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    struct Queue final {

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
         * @brief Queue name
         */
        std::string name;

        /**
         * @brief Queue ERN
         */
        std::string ern;

        /**
         * @brief Queue attributes
         */
        QueueAttribute attributes;

        /**
         * @brief Queue tags
         */
        std::map<std::string, std::string> tags;

        /**
         * @brief Queue size in bytes
         */
        long size{};

        /**
         * @brief Delay in seconds
         */
        long delay{};

        /**
         * @brief Total number of messages
         */
        long available{};

        /**
         * @brief Number of delayed messages
         */
        long delayed{};

        /**
         * @brief Number of invisible messages
         */
        long invisible{};

        /**
         * @brief Visibility in seconds
         */
        long visibility = 30;

        /**
         * @brief Maximal message length in bytes
         */
        long maxMessageLength = 1024 * 1024;

        /**
         * @brief Maximal receive count
         */
        long maxReceiveCount = 3;

        /**
         * @brief Maximal receive count
         */
        std::string deadLetterQueueErn{};

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
        static Queue fromDocument(const std::optional<bsoncxx::document::view> &document);
    };

} // namespace Euclid::Database::Entity::SQS