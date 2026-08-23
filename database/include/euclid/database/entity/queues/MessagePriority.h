//
// Created by vogje01 on 8/19/26.
//

#pragma once

// C++ includes
#include <algorithm>
#include <map>
#include <string>

namespace Euclid::Database::Entity::Queues {

    /**
     * @brief SQS message priority
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    enum class MessagePriority {
        LOW,
        MIDDLE,
        HIGH
    };

    static std::map<MessagePriority, std::string> MessagePriorityNames{
            {MessagePriority::LOW, "LOW"},
            {MessagePriority::MIDDLE, "MIDDLE"},
            {MessagePriority::HIGH, "HIGH"},
    };

    [[maybe_unused]]
    static std::string MessagePriorityToString(const MessagePriority &priority) {
        return MessagePriorityNames[priority];
    }

    /**
     * @brief Parses a message priority from its string representation.
     *
     * Unrecognized or empty input (e.g. a message sent before this attribute existed, or a
     * request that didn't set it) falls back to the documented default of MIDDLE, rather than an
     * UNKNOWN state.
     */
    [[maybe_unused]]
    static MessagePriority MessagePriorityFromString(const std::string &priority) {
        const auto it = std::ranges::find_if(MessagePriorityNames, [&priority](const auto &pair) { return pair.second == priority; });
        return it != MessagePriorityNames.end() ? it->first : MessagePriority::MIDDLE;
    }

}// namespace Euclid::Database::Entity::SQS