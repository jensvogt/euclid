// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

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
#include <euclid/database/entity/com/Variant.h>
#include <euclid/database/entity/eqs/MessagePriority.h>
#include <euclid/database/entity/eqs/MessageStatus.h>

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
         * @brief Queue this message was moved to a dead letter queue from, or empty if it was
         * never moved.
         *
         * @par
         * A message reaches a dead letter queue by having its queueErn rewritten, which leaves
         * nothing behind saying where it had been - and "put it back" is the one thing anybody
         * ever wants to do with a dead letter queue. Recorded here so redrive-dlq can return each
         * message to the queue it actually failed in, which matters when several queues share one
         * dead letter queue and a single "original queue" does not exist.
         */
        std::string sourceQueueErn;

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
         * @brief Point in time at which this message is removed whether it was consumed or not.
         *
         * Set at send time to now()+queue.retentionPeriod, and acted on by the TTL index
         * MongoEqsRepository::ensureIndexes() puts on this field - so the deletion happens in the
         * database, on its own schedule, and costs the send nothing but the stamp.
         *
         * A message written before retention existed carries no expiry, and a TTL index ignores a
         * document whose field is absent: those messages are left alone rather than swept up by a
         * deployment. Emptying a queue that predates this is a deliberate act, not a side effect
         * of upgrading.
         */
        system_clock::time_point expiresAt;

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
         * @brief List of message attributes.
         *
         * These are the user-contributed message attributes.
         */
        std::map<std::string, COM::Variant> attributes;

        /**
         * @brief Euclid's own attributes, carried across every hop and never mixed into the
         * user's.
         *
         * @par
         * The envelope, as opposed to the contents. Something that decides work is urgent, or that
         * gives it a correlation id, has to be able to say so where the fact survives being written
         * to a bucket and read back out as a queue message - and a bucket has no notion of urgency
         * of its own. Without somewhere to put it, that decision has to be smuggled through the one
         * thing that does travel, which is why a key prefix like "LowPriority/" ends up encoding
         * something that is not about the key at all.
         *
         * @par
         * Kept apart from the user's attributes rather than reserving names inside them: a caller
         * listing or setting attributes sees only its own, so euclid can add a system attribute
         * later without colliding with something somebody already stores, and a user attribute
         * called "priority" means nothing to euclid.
         */
        std::map<std::string, COM::Variant> systemAttributes;

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
        system_clock::time_point modified = system_clock::now();

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

    };

}// namespace Euclid::Database::Entity::SQS