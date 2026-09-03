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
#include "MessagePriority.h"
#include "QueueStatus.h"

#include <euclid/database/entity/BaseEntity.h>
#include <euclid/database/entity/eqs/QueueAttribute.h>

namespace Euclid::Database::Entity::EQS {

    using std::chrono::system_clock;

    /**
     * @brief SQS queue entity
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    struct Queue final : BaseEntity {

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
         * @brief Default priority for messages
         */
        MessagePriority priority = MessagePriority::MIDDLE;

        /**
         * @brief Whether this queue is euclid's own plumbing rather than a user's queue.
         *
         * @par
         * An internal queue is an ordinary queue in every respect that matters - messages are sent,
         * received, made invisible, redriven and deleted exactly the same way - except that it is
         * left out of list-queues and the queue count, so nothing offers it to somebody who did not
         * create it and cannot act on it. It exists because a delivery has to land somewhere: a
         * bucket subscription, for instance, needs a queue to fan out into, and that queue is an
         * implementation detail of whoever registered the subscription.
         *
         * @par
         * Hidden, not protected. A caller that knows the ERN can still use it, which is exactly
         * what the component that created it does.
         */
        bool internal = false;

        /**
         * @brief Whether this queue may be received from.
         *
         * @par
         * Sending is unaffected: a STOPPED queue still accepts everything producers write to it,
         * and messages accumulate exactly as they would while no consumer happened to be running.
         * Only receive-messages is refused, so a consumer cannot take work out while somebody is
         * fixing, draining or investigating it - and, being a queue-level status rather than an
         * access rule, it stops every consumer at once rather than one identity at a time.
         *
         * @par
         * Refused rather than answered with an empty list, deliberately: a stopped queue that read
         * as empty would be indistinguishable from a queue nobody is writing to, which is exactly
         * the distinction whoever stopped it needs to be able to make.
         */
        QueueStatus status = QueueStatus::AVAILABLE;

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

}// namespace Euclid::Database::Entity::SQS