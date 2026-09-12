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
     * @brief The retention period that keeps a published message forever.
     *
     * @par
     * Minus one rather than zero, because zero is already taken and means the opposite of a
     * decision: a topic that has never been told what it wants and follows the installation. This
     * is a decision - keep everything published here - and a topic that has made it does not follow
     * a later change to the installation's period either.
     *
     * @par
     * What it does is leave Message::expiresAt unset, which is exactly what a message published
     * before retention existed looks like, and what the TTL index ignores. So "forever" is not a
     * very large number that quietly comes due in 2098; it is the absence of an expiry, and the
     * database is never asked to remove the message at all.
     */
    constexpr long kRetentionForever = -1;

    /**
     * @brief The largest message a topic accepts when it has not been given a limit of its own,
     * in bytes.
     *
     * @par
     * One mebibyte, the same figure EQS uses for a queue message and the same one create-topic
     * defaults to. Named because three places have to agree on it: the entity's own default, what a
     * create request falls back to when the caller omits the field, and what a publish measures
     * against for a topic stored before the limit meant anything.
     */
    constexpr long kDefaultMaxMessageLength = 1024 * 1024;

    /**
     * @brief The limit a publish is actually measured against.
     *
     * @par
     * A topic carrying no limit of its own - zero, which is what a create-topic that omitted the
     * field used to store - is measured against the default rather than refusing everything. Named
     * rather than written out at the one place that checks it, because "zero means unset" is a rule
     * about the data and not about the handler that happens to read it.
     *
     * @param configured what the topic holds.
     * @return the limit in bytes, always positive.
     */
    constexpr long EffectiveMaxMessageLength(const long configured) {
        return configured > 0 ? configured : kDefaultMaxMessageLength;
    }

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
         * @brief Maximal message length in bytes.
         *
         * @par
         * Zero means no limit of this topic's own, which is what a topic created before the field
         * was sent, or by a client that omitted it, holds. A publish measures against
         * kDefaultMaxMessageLength in that case rather than refusing everything - see
         * set-topic-max-message-length for giving the topic a limit it means.
         */
        long maxMessageLength = kDefaultMaxMessageLength;

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
         * {@link kRetentionForever} (-1) keeps every message published to this topic, by stamping
         * no expiry on it at all. It is the one value that opts out of retention rather than
         * choosing a length of it, which is why it is a sign rather than a large number.
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