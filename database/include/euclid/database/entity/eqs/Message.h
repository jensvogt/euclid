//
// Created by vogje01 on 01/06/2023.
//

#pragma once

// C++ includes
#include <chrono>
#include <map>
#include <string>

// MongoDB includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/document/value.hpp>

// Euclid includes
#include <euclid/database/entity/eqs/MessagePriority.h>
#include <euclid/database/entity/eqs/MessageStatus.h>
#include <euclid/database/entity/eqs/Variant.h>

namespace Euclid::Database::Entity::EQS {

    using std::chrono::system_clock;

    /**
     * @brief EQS message entity
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    struct Message {

        /**
         * @brief ID
         */
        std::string oid;

        /**
         * @brief Message ern
         */
        std::string ern;

        /**
         * @brief Queue ERN
         */
        std::string queueErn;

        /**
         * @brief Message body
         */
        std::string body;

        /**
         * @brief Status
         */
        MessageStatus status = MessageStatus::AVAILABLE;

        /**
         * @brief Priority
         *
         * Used by receiveMessages() to favor higher priority messages over lower priority ones;
         * see IEqsRepository::receiveMessages() for details. Defaults to MIDDLE.
         */
        MessagePriority priority = MessagePriority::MIDDLE;

        /**
         * @brief Last send datetime
         */
        system_clock::time_point reset;

        /**
         * @brief Point in time at which a DELAYED message becomes AVAILABLE.
         *
         * Set at send time to now()+queue.delay when the owning queue has a delay configured.
         * Ignored once the message is no longer in status DELAYED.
         */
        system_clock::time_point delayUntil;

        /**
         * @brief Number of times this message has been received (ApproximateReceiveCount).
         *
         * Incremented every time the message is handed out by receiveMessages(). Once it exceeds
         * the owning queue's maxReceiveCount, the message is moved to the queue's dead letter
         * queue (if one is configured) instead of being redelivered.
         */
        long receivedCount{};

        /**
         * @brief Message size in bytes
         */
        long size{};

        /**
         * @brief Visibility timeout in seconds
         */
        long visibilityTimeout = 30;

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
        std::map<std::string, Variant> attributes;

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
        static std::string ComputeAttributesMd5(const std::map<std::string, Variant> &attributes);
    };

}// namespace Euclid::Database::Entity::SQS