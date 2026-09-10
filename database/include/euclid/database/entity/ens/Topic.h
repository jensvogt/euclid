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
#include <euclid/database/entity/ens/TopicAttribute.h>

namespace Euclid::Database::Entity::ENS {

    using std::chrono::system_clock;

    /**
     * @brief ENS topic entity
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    /**
     * @brief How long a published message is kept when neither the topic nor the configuration
     * says otherwise, in seconds.
     *
     * Fourteen days. Longer than EQS keeps a queue message, because the two are not the same
     * thing: a queue message is consumed and gone, while a topic message is fanned out at publish
     * time and what stays behind is a record of what was published. Two weeks is long enough to
     * answer "what did we send them" after a quiet fortnight, and short enough that the answer
     * does not have to be kept forever.
     */
    constexpr long kDefaultRetentionPeriod = 14 * 24 * 60 * 60;

    /**
     * @brief A topic that hands what is published to it to its subscribers.
     */
    constexpr auto kStatusRunning = "RUNNING";

    /**
     * @brief A topic that is holding what is published to it instead - see Topic::delivering.
     */
    constexpr auto kStatusStopped = "STOPPED";

    struct Topic final : BaseEntity {

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
        TopicAttribute attributes;

        /**
         * @brief Queue tags
         */
        std::map<std::string, std::string> tags;

        /**
         * @brief Queue size in bytes
         */
        long size{};

        /**
         * @brief Total number of messages
         */
        long available{};

        /**
         * @brief Total number of messages send
         */
        long send{};

        /**
         * @brief Total number of messages resend
         */
        long resend{};

        /**
         * @brief Maximal message length in bytes
         */
        long maxMessageLength = 1024 * 1024;

        /**
         * @brief Whether messages published to this topic are handed to its subscribers.
         *
         * @par
         * A stopped topic still accepts and stores what is published to it - it simply does not
         * fan it out. That is the point: a subscriber being redeployed, or a downstream system
         * taken down for the evening, is a reason to hold delivery rather than to lose what
         * arrives meanwhile. Those messages are kept with status
         * Entity::ENS::kStatusHeld and delivered, oldest first, when the topic is started again.
         *
         * @par
         * True by default and absent from a topic written before this existed, which a document
         * read leaves at that default - so every topic that already exists keeps delivering.
         */
        bool delivering = true;

        /**
         * @brief How long a message published to this topic is kept, in seconds.
         *
         * @par
         * Published messages were kept forever. A topic is fanned out at publish time, so nothing
         * ever consumed them and nothing ever removed them: the collection only grew, and because
         * every topic shares it, the cost of one busy topic was paid by every publish in the
         * installation.
         *
         * @par
         * Zero rather than the default itself, so that a topic which has never been told what it
         * wants follows {@code euclid.modules.ens.retention-period} as it changes, instead of
         * having frozen a copy of whatever the default was on the day it was created. A topic that
         * has been given a period of its own keeps it - see the set-topic-retention action.
         *
         * @par
         * Enforced by the database rather than by a sweep: each message is stamped with when it
         * expires and a TTL index removes it after that - see
         * MongoEnsRepository::ensureIndexes(). Changing this affects messages published
         * afterwards; the ones already stored keep the expiry they were given.
         */
        long retentionPeriod = 0;

        /**
         * @brief Creation date
         */
        system_clock::time_point created = system_clock::now();

        /**
         * @brief Last modification date
         */
        system_clock::time_point modified = system_clock::now();

        /**
         * @brief What state the topic is in, as the API reports it: RUNNING or STOPPED.
         *
         * @par
         * Derived rather than stored. Whether a topic delivers is one bit, and a stored word
         * saying the same thing is a second copy of it that can disagree with the first - so the
         * flag is the truth and this is how it reads.
         */
        [[nodiscard]]
        std::string status() const { return delivering ? kStatusRunning : kStatusStopped; }

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
        static Topic fromDocument(const std::optional<bsoncxx::document::view> &document);
    };

}// namespace Euclid::Database::Entity::SQS