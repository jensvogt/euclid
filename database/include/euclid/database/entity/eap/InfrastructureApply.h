// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <map>
#include <string>
#include <vector>

// Boost includes
#include <boost/json.hpp>

// Euclid includes
#include <euclid/database/entity/ens/Topic.h>
#include <euclid/database/entity/eqs/MessagePriority.h>
#include <euclid/database/entity/eqs/Queue.h>

namespace Euclid::Database::Entity::EAP::Infrastructure {

    /**
     * @brief The tag that says which application owns an object.
     *
     * @par
     * The same tag `eap apply` has always written, so an object it tagged is recognised as owned
     * here and not taken for somebody else's. Written when a declaration is applied rather than when
     * the object is created, which is what keeps euclid-spring's per-run delivery queues - created by
     * the listener container, declared by nobody - out of reach of a prune.
     */
    inline constexpr auto kOwnerTag = "euclid:application";

    /**
     * @brief What applying a `creates` entry does to the object of that name.
     */
    enum class Claim {
        /// Not there: make it.
        Create,
        /// There, and tagged as this application's.
        Mine,
        /// There and untagged: made by hand, or before the tag existed. Recorded as this
        /// application's - the one step that takes something over, so it is reported as its own.
        Adopt,
        /// There and another application's. Refused: the deploy path used to record such an
        /// object as owned, and the owner's next reconcile could then delete it out from under it.
        Clash
    };

    inline Claim ClaimOn(const bool exists, const std::map<std::string, std::string> &tags, const std::string &applicationId) {
        if (!exists) return Claim::Create;
        const auto owner = tags.find(kOwnerTag);
        if (owner == tags.end() || owner->second.empty()) return Claim::Adopt;
        return owner->second == applicationId ? Claim::Mine : Claim::Clash;
    }

    /**
     * @brief Whether an object this application no longer declares is its own to remove.
     *
     * @param tags          the object's tags.
     * @param applicationId the application.
     * @param recorded      whether the application's record lists it - what an object made by the
     *                      deploy path before the tag existed has instead of a tag.
     */
    inline bool MayRemove(const std::map<std::string, std::string> &tags, const std::string &applicationId, const bool recorded) {
        const auto owner = tags.find(kOwnerTag);
        if (owner == tags.end() || owner->second.empty()) return recorded;
        return owner->second == applicationId;
    }

    namespace detail {

        inline void setLong(long &field, const boost::json::object &settings, const char *key, const char *label,
                            std::vector<std::string> &changes) {
            const auto *value = settings.if_contains(key);
            if (value == nullptr || !value->is_int64()) return;
            const auto wanted = static_cast<long>(value->as_int64());
            if (field == wanted) return;
            changes.push_back(std::string(label) + " " + std::to_string(field) + " -> " + std::to_string(wanted));
            field = wanted;
        }

    }// namespace detail

    /**
     * @brief Applies a `creates` entry's settings to a queue, and says what changed.
     *
     * @par
     * Only what the entry names. A setting left out keeps whatever the queue has - its default on a
     * new queue, its current value on an existing one - so removing a line from the file never
     * resets a value somebody changed by hand.
     *
     * @param queue    the queue, existing or about to be made.
     * @param settings the entry's settings, already checked by SettingsProblem().
     * @param deadLetterQueueErn the ERN `deadLetterQueue` resolved to, or empty when it names none.
     * @return one line per change, e.g. "visibility 30 -> 900".
     */
    inline std::vector<std::string> ApplyQueueSettings(EQS::Queue &queue, const boost::json::object &settings,
                                                       const std::string &deadLetterQueueErn = {}) {
        std::vector<std::string> changes;
        detail::setLong(queue.visibility, settings, "visibility", "visibility", changes);
        detail::setLong(queue.maxReceiveCount, settings, "maxRetries", "maxRetries", changes);
        detail::setLong(queue.maxMessageLength, settings, "maxMessageLength", "maxMessageLength", changes);
        detail::setLong(queue.delay, settings, "delay", "delay", changes);
        detail::setLong(queue.retentionPeriod, settings, "retention", "retention", changes);

        if (const auto *priority = settings.if_contains("priority"); priority != nullptr && priority->is_string()) {
            if (const auto wanted = EQS::TryMessagePriorityFromString(std::string(priority->as_string()));
                wanted.has_value() && *wanted != queue.priority) {
                changes.push_back("priority " + EQS::MessagePriorityToString(queue.priority) + " -> " + EQS::MessagePriorityToString(*wanted));
                queue.priority = *wanted;
            }
        }
        if (settings.contains("deadLetterQueue") && !deadLetterQueueErn.empty() && queue.deadLetterQueueErn != deadLetterQueueErn) {
            changes.push_back("deadLetterQueue " + (queue.deadLetterQueueErn.empty() ? std::string("none") : queue.deadLetterQueueErn)
                              + " -> " + deadLetterQueueErn);
            queue.deadLetterQueueErn = deadLetterQueueErn;
        }
        return changes;
    }

    /**
     * @brief Applies a `creates` entry's settings to a topic, and says what changed. The same rule
     * as ApplyQueueSettings(): only what the entry names.
     */
    inline std::vector<std::string> ApplyTopicSettings(ENS::Topic &topic, const boost::json::object &settings) {
        std::vector<std::string> changes;
        detail::setLong(topic.maxMessageLength, settings, "maxMessageLength", "maxMessageLength", changes);
        detail::setLong(topic.retentionPeriod, settings, "retention", "retention", changes);
        return changes;
    }

}// namespace Euclid::Database::Entity::EAP::Infrastructure
