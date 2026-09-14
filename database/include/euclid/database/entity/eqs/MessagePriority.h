// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/19/26.
//

#pragma once

// C++ includes
#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <string>

namespace Euclid::Database::Entity::EQS {

    /**
     * @brief The system attribute a producer sets to say how urgent the work is.
     *
     * @par
     * Named here, beside the values it takes, because it is the one system attribute euclid itself
     * acts on - everything else in that map is carried and not read. A producer two hops upstream
     * sets it on the object it writes; the queue the notification lands in reads it here.
     */
    constexpr auto kPriorityAttribute = "priority";

    /**
     * @brief SQS message priority
     *
     * @author jensvogt47\@gmail.com
     */
    enum class MessagePriority {
        LOW,
        MEDIUM,
        HIGH
    };

    static std::map<MessagePriority, std::string> MessagePriorityNames{
            {MessagePriority::LOW, "LOW"},
            {MessagePriority::MEDIUM, "MEDIUM"},
            {MessagePriority::HIGH, "HIGH"},
    };

    /**
     * @brief What the middle tier used to be called, still accepted when read.
     *
     * @par
     * The name changed to MEDIUM; the value did not. Every message stored before that carries this
     * string, and so does every request from a client built against an older SDK - euclid's SDKs
     * are released separately and an installation is routinely a version ahead of them. Refusing it
     * would turn a rename into an outage for those callers and would make a stored row unreadable.
     *
     * @par
     * Read-only. Nothing writes it: MessagePriorityToString() answers MEDIUM for every message,
     * including the ones stored as MIDDLE, so the old spelling leaves the installation as the rows
     * carrying it are replaced.
     */
    constexpr auto kLegacyMediumName = "MIDDLE";

    [[maybe_unused]]
    static std::string MessagePriorityToString(const MessagePriority &priority) {
        return MessagePriorityNames[priority];
    }

    /**
     * @brief Reads a priority, or nothing if the string names none.
     *
     * @par
     * Case-insensitive: "low" typed by a person and "LOW" as it is stored mean the same thing, and
     * refusing one of them teaches nobody anything.
     *
     * @par
     * Returning nothing rather than a default is what lets the two kinds of caller differ. A
     * request carrying a typo should be told - silently sending at MEDIUM what somebody asked to
     * send at LOW is a decision made on their behalf and never reported. A value read back from the
     * database should not fail at all, since a row that cannot be parsed is still a message
     * somebody is waiting for; MessagePriorityFromString() is that reading.
     */
    [[maybe_unused]]
    static std::optional<MessagePriority> TryMessagePriorityFromString(const std::string &priority) {
        auto upper = priority;
        std::ranges::transform(upper, upper.begin(), [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });

        // Before the table, so the old name resolves to the tier it always meant rather than
        // falling through to "no such priority" - see kLegacyMediumName.
        if (upper == kLegacyMediumName) return MessagePriority::MEDIUM;

        const auto it = std::ranges::find_if(MessagePriorityNames, [&upper](const auto &pair) { return pair.second == upper; });
        if (it == MessagePriorityNames.end()) return std::nullopt;
        return it->first;
    }

    /**
     * @brief Reads a stored priority, defaulting to MEDIUM.
     *
     * @par
     * A value nobody can parse has to land somewhere sensible rather than at the bottom of the
     * queue, which is where LOW would put it. Used for values already stored and for a delivery in
     * flight - anywhere failing would cost a message. Requests use TryMessagePriorityFromString()
     * and refuse instead.
     */
    static MessagePriority MessagePriorityFromString(const std::string &priority) {
        return TryMessagePriorityFromString(priority).value_or(MessagePriority::MEDIUM);
    }

}// namespace Euclid::Database::Entity::SQS