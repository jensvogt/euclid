//
// Created by vogje01 on 8/19/26.
//

#pragma once

// C++ includes
#include <map>

// Euclid includes
#include <euclid/database/entity/sqs/MessagePriority.h>

namespace Euclid::Database::Entity::SQS {

    /**
     * @brief Relative weight of each priority tier when apportioning receiveMessages() slots.
     *
     * Defaults to 4:2:1 for HIGH:MIDDLE:LOW, i.e. every step down in priority halves the share of
     * slots it gets - the discrete analogue of a logarithmic priority curve with only three tiers.
     * Overridable per-deployment via euclid.modules.sqs.priority-weights.{high,middle,low} in the
     * configuration file.
     */
    struct PriorityWeights {
        double high = 4.0;
        double middle = 2.0;
        double low = 1.0;
    };

    /**
     * @brief Loads the priority weights from the configuration, falling back to the defaults for
     * any tier that isn't explicitly configured.
     *
     * @return the configured (or default) priority weights.
     */
    [[nodiscard]]
    PriorityWeights LoadPriorityWeights();

    /**
     * @brief Splits maxCount receiveMessages() slots across the three priority tiers.
     *
     * Slots are apportioned proportionally to `weights` using the largest-remainder method, then
     * capped by how many messages of each tier are actually available. Any shortfall left by a
     * tier that doesn't have enough messages is redistributed to the other tiers, highest priority
     * first, so as many of the maxCount slots as possible get filled.
     *
     * @param maxCount total number of messages requested.
     * @param available number of AVAILABLE messages per priority tier, for the queue being polled.
     * @param weights relative weight of each tier.
     * @return number of messages to take from each tier; the values sum to at most maxCount and to
     * at most the total number of available messages.
     */
    [[nodiscard]]
    std::map<MessagePriority, long> ComputeReceiveCounts(long maxCount, const std::map<MessagePriority, long> &available, const PriorityWeights &weights);

}// namespace Euclid::Database::Entity::SQS
