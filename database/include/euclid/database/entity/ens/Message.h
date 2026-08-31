//
// Created by vogje01 on 01/06/2023.
//

#pragma once

// C++ includes
#include <chrono>
#include <map>
#include <string>

// MongoDB includes
#include <bsoncxx/json.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/document/value.hpp>

// Euclid includes
#include <euclid/core/CryptoUtils.h>
#include <euclid/database/entity/BaseEntity.h>
#include <euclid/database/entity/com/Variant.h>

namespace Euclid::Database::Entity::ENS {

    using std::chrono::system_clock;

    /**
     * @brief ENS message entity
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    struct Message final : BaseEntity {

        /**
         * @brief ID
         */
        std::string oid;

        /**
         * @brief Message ern
         */
        std::string ern;

        /**
         * @brief Topic ERN
         */
        std::string topicErn;

        /**
         * @brief Message body
         */
        std::string body;

        /**
         * @brief Message size in bytes
         */
        long size{};

        /**
         * @brief Timestamp of the last time this message was claimed by receiveMessages().
         *
         * Together with visibilityTimeout, used to detect when an in-flight (status INVISIBLE)
         * message's visibility timeout has expired, so it can be made receivable again. Reset to
         * the epoch once the message is returned to status INITIAL.
         */
        system_clock::time_point lastReceived;

        /**
         * @brief Message ID
         */
        std::string messageId;

        /**
         * @brief Receipt handle
         */
        std::string receiptHandle;

        /**
         * @brief MD5 sum body
         */
        std::string md5Body;

        /**
         * @brief List of message attributes.
         *
         * These are the user-contributed message attributes.
         */
        std::map<std::string, COM::Variant> attributes;

        /**
         * @brief MD5 sum of the message attributes.
         *
         * Computed from the attributes map when the message is sent, so callers can detect
         * whether the attributes were transmitted/stored correctly.
         */
        std::string md5Attributes;

        /**
         * @brief Content type
         */
        std::string contentType;

        /**
         * @brief Delivery status
         *
         * ENS is fire-and-forget pub/sub (no consumer-side visibility timeout/claim cycle like
         * EQS), so this is a single terminal value set once at publish time rather than a state
         * machine.
         */
        std::string status = "PUBLISHED";

        /**
         * @brief Creation date
         */
        system_clock::time_point created = system_clock::now();

        /**
         * @brief Last modification date
         */
        system_clock::time_point modified;

        /**
         * @brief Converts the entity to a MongoDB document
         *
         * @return entity as a MongoDB document.
         */
        [[nodiscard]]
        bsoncxx::document::value ToDocument() const;

        /**
         * @brief Converts the MongoDB document to an entity
         *
         * @param document MongoDB document.
         */
        void FromDocument(const std::optional<bsoncxx::document::view> &document);

        /**
         * @brief Computes the MD5 sum of a message attributes map.
         *
         * @param attributes message attributes to hash.
         * @return MD5 sum, as a hex string.
         */
        [[nodiscard]]
        static std::string ComputeAttributesMd5(const std::map<std::string, COM::Variant> &attributes);
    };

}// namespace Euclid::Database::Entity::SQS