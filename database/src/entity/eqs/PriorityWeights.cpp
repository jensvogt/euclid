// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/19/26.
//

#include <euclid/database/entity/eqs/PriorityWeights.h>

// C++ includes
#include <algorithm>
#include <array>
#include <cmath>

// Euclid includes
#include <euclid/core/Configuration.h>

namespace Euclid::Database::Entity::EQS {

    PriorityWeights LoadPriorityWeights() {
        auto &cfg = Core::Configuration::instance();
        PriorityWeights weights;
        weights.high = cfg.getOr<double>("euclid.modules.eqs.priority-weights.high", weights.high);

        // The middle tier is MEDIUM now and this key is ".medium". An installation that set the old
        // ".middle" keeps the weight it chose: a renamed key that silently reverts to the default
        // is a tuning decision undone by an upgrade, and nothing would say so - the queue would
        // simply start apportioning slots differently.
        const auto legacyMedium = cfg.getOr<double>("euclid.modules.eqs.priority-weights.medium", weights.medium);
        weights.medium = cfg.getOr<double>("euclid.modules.eqs.priority-weights.medium", legacyMedium);

        weights.low = cfg.getOr<double>("euclid.modules.eqs.priority-weights.low", weights.low);
        return weights;
    }

    std::map<MessagePriority, long> ComputeReceiveCounts(const long maxCount, const std::map<MessagePriority, long> &available, const PriorityWeights &weights) {

        // Highest priority first, so leftover slots spill downward before upward.
        constexpr std::array<MessagePriority, 3> order{MessagePriority::HIGH, MessagePriority::MEDIUM, MessagePriority::LOW};

        std::map<MessagePriority, long> take{{MessagePriority::HIGH, 0}, {MessagePriority::MEDIUM, 0}, {MessagePriority::LOW, 0}};
        if (maxCount <= 0) return take;

        const std::array<double, 3> w{weights.high, weights.medium, weights.low};
        const double totalWeight = w[0] + w[1] + w[2];
        if (totalWeight <= 0) return take;

        // Proportional apportionment, largest-remainder method.
        std::array<long, 3> target{};
        std::array<double, 3> remainder{};
        long allocated = 0;
        for (std::size_t i = 0; i < 3; ++i) {
            const double raw = static_cast<double>(maxCount) * w[i] / totalWeight;
            target[i] = static_cast<long>(std::floor(raw));
            remainder[i] = raw - static_cast<double>(target[i]);
            allocated += target[i];
        }

        std::array<std::size_t, 3> byRemainder{0, 1, 2};
        std::ranges::stable_sort(byRemainder, [&](const std::size_t a, const std::size_t b) { return remainder[a] > remainder[b]; });
        for (std::size_t i = 0; i < 3 && allocated < maxCount; ++i) {
            ++target[byRemainder[i]];
            ++allocated;
        }

        // Cap by what's actually available, redistributing any shortfall highest-priority-first.
        std::array<long, 3> avail{};
        for (std::size_t i = 0; i < 3; ++i) {
            const auto it = available.find(order[i]);
            avail[i] = it != available.end() ? it->second : 0;
        }

        std::array<long, 3> taken{};
        long shortfall = 0;
        for (std::size_t i = 0; i < 3; ++i) {
            taken[i] = std::min(target[i], avail[i]);
            shortfall += target[i] - taken[i];
        }
        for (std::size_t i = 0; i < 3 && shortfall > 0; ++i) {
            const long spare = std::min(avail[i] - taken[i], shortfall);
            taken[i] += spare;
            shortfall -= spare;
        }

        take[order[0]] = taken[0];
        take[order[1]] = taken[1];
        take[order[2]] = taken[2];
        return take;
    }

}// namespace Euclid::Database::Entity::SQS