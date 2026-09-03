//
// Created by vogje01 on 9/4/26.
//

#pragma once

// C++ includes
#include <algorithm>
#include <map>
#include <string>

namespace Euclid::Database::Entity::EQS {

    /**
     * @brief Whether a queue may be received from.
     *
     * @par
     * Only consumption is affected. A STOPPED queue accepts everything producers send it and goes
     * on accumulating messages exactly as an AVAILABLE one would while no consumer happened to be
     * running - what changes is that receive-messages is refused rather than answered.
     *
     * @par
     * An enum rather than a flag because a queue's state is the kind of thing that acquires more
     * values: draining, quarantined, read-only. Two names now, with the string stored rather than
     * an ordinal, is what makes a third one cost nothing later.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    enum class QueueStatus {
        AVAILABLE,
        STOPPED,
        UNKNOWN
    };

    static std::map<QueueStatus, std::string> QueueStatusNames{
            {QueueStatus::AVAILABLE, "AVAILABLE"},
            {QueueStatus::STOPPED, "STOPPED"},
            {QueueStatus::UNKNOWN, "UNKNOWN"},
    };

    [[maybe_unused]]
    static std::string QueueStatusToString(const QueueStatus &queueStatus) {
        return QueueStatusNames[queueStatus];
    }

    /**
     * @brief Reads a stored status, defaulting to AVAILABLE rather than UNKNOWN.
     *
     * @par
     * Unlike a message status, this is absent from every queue written before the status existed -
     * and those queues were all receivable. Reading a missing or unrecognised value as AVAILABLE
     * is what keeps them that way; UNKNOWN would silently stop every queue in an installation the
     * moment it upgraded.
     */
    [[maybe_unused]]
    static QueueStatus QueueStatusFromString(const std::string &queueStatus) {
        const auto it = std::ranges::find_if(QueueStatusNames, [&queueStatus](const auto &pair) { return pair.second == queueStatus; });
        return it != QueueStatusNames.end() ? it->first : QueueStatus::AVAILABLE;
    }

}// namespace Euclid::Database::Entity::EQS
