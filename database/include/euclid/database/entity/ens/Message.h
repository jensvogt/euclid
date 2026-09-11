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
     * @brief A message that has been handed to the topic's subscribers.
     */
    constexpr auto kStatusPublished = "PUBLISHED";

    /**
     * @brief A message stored while its topic was stopped, waiting to be delivered when it is
     * started again - see Entity::ENS::Topic::delivering.
     */
    constexpr auto kStatusHeld = "HELD";

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
         * @brief List of message attributes.
         *
         * These are the user-contributed message attributes.
         */
        std::map<std::string, COM::Variant> attributes;

        /**
         * @brief Euclid's own attributes, carried across every hop and never mixed into the
         * user's - see Entity::EQS::Message::systemAttributes. A topic in the middle of a chain
         * must not be where the envelope stops.
         */
        std::map<std::string, COM::Variant> systemAttributes;


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
         * @brief The priority the publisher asked for.
         *
         * @par
         * It means nothing to the topic - a topic is not consumed from - and everything to the
         * queues the message is fanned out to. Kept with the message because a held message is
         * delivered later, and a replay that dropped the priority would quietly turn urgent work
         * into ordinary work.
         */
        std::string priority = "MIDDLE";

        /**
         * @brief When this message stops being kept.
         *
         * @par
         * Set at publish time to now() plus the topic's retention period, and acted on by the TTL
         * index on this field - so the removing is the database's, not a sweep of euclid's.
         *
         * @par
         * A message published before retention existed carries no expiry, and a TTL index ignores
         * a document whose field is absent: those are left alone rather than swept up by a
         * deployment. Clearing out what a topic collected before that is a deliberate act, not a
         * side effect of upgrading.
         */
        system_clock::time_point expiresAt{};

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

    };

}// namespace Euclid::Database::Entity::SQS