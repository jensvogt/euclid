// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <algorithm>
#include <cstdint>
#include <optional>

namespace Euclid::Monitoring {

    /**
     * @brief How full a filesystem is, as a percentage of its total size - the disk-used-percent
     * metric.
     *
     * @par
     * Used is everything euclid cannot write to: the total less what is available. That counts the
     * blocks the filesystem keeps back for root as used, which is what makes 100% mean "no more
     * writes" - measured from free space instead, an ext4 volume with the usual 5% reserve would
     * stop at 95% with every write already failing, and an alert at 98% would never fire.
     *
     * @param capacity the filesystem's total size, in bytes
     * @param available the bytes an unprivileged process can still write
     * @return 0 to 100, or nothing for a filesystem that reports no size, which has no percentage
     */
    [[nodiscard]] inline std::optional<double> DiskUsedPercent(const std::uintmax_t capacity, const std::uintmax_t available) {
        if (capacity == 0) return std::nullopt;
        const auto used = capacity - std::min(available, capacity);
        return 100.0 * static_cast<double>(used) / static_cast<double>(capacity);
    }

}// namespace Euclid::Monitoring
