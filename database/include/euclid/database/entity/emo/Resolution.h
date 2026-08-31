//
// Created by vogje01 on 8/31/26.
//

#pragma once

// C++ includes
#include <algorithm>
#include <chrono>
#include <map>
#include <string>

namespace Euclid::Database::Entity::Monitoring {

    /**
     * @brief Storage resolution of a monitoring data point.
     *
     * @par
     * Monitoring data is kept in three tiers of decreasing resolution and increasing retention,
     * so that a year of history costs a few thousand rows per series instead of a few hundred
     * thousand. RAW rows are written by the monitoring module's flush task, HOUR rows are rolled
     * up from RAW and DAY rows from HOUR - each tier only ever reads the tier above it.
     *
     * @par
     * RAW has no fixed width: it is whatever euclid.monitoring.average-period is set to (300s by
     * default), which is why the bucket width is not part of this enum. Use ResolutionBucket() for
     * the two derived tiers and the configured period for RAW.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    enum class Resolution {
        RAW,
        HOUR,
        DAY,
        UNKNOWN
    };

    static std::map<Resolution, std::string> ResolutionNames{
            {Resolution::RAW, "RAW"},
            {Resolution::HOUR, "HOUR"},
            {Resolution::DAY, "DAY"},
            {Resolution::UNKNOWN, "UNKNOWN"},
    };

    [[maybe_unused]]
    static std::string ResolutionToString(const Resolution &resolution) {
        return ResolutionNames[resolution];
    }

    [[maybe_unused]]
    static Resolution ResolutionFromString(const std::string &resolution) {
        const auto it = std::ranges::find_if(ResolutionNames, [&resolution](const auto &pair) { return pair.second == resolution; });
        return it != ResolutionNames.end() ? it->first : Resolution::UNKNOWN;
    }

    /**
     * @brief Returns the bucket width of a derived resolution.
     *
     * @param resolution resolution to get the bucket width for.
     * @return bucket width, or zero for RAW and UNKNOWN, whose width is the configured
     * euclid.monitoring.average-period rather than a fixed value.
     */
    [[maybe_unused]]
    static std::chrono::seconds ResolutionBucket(const Resolution &resolution) {
        switch (resolution) {
            case Resolution::HOUR:
                return std::chrono::hours(1);
            case Resolution::DAY:
                return std::chrono::hours(24);
            default:
                return std::chrono::seconds(0);
        }
    }

    /**
     * @brief Floors a timestamp to the start of the bucket containing it.
     *
     * @par
     * Bucket boundaries are counted from the Unix epoch, which is itself an exact UTC midnight,
     * so this yields whole hours and whole UTC days for the derived resolutions. Aligning is what
     * makes a rollup idempotent: recomputing the same window always targets the same bucket, so a
     * rollup can be re-run after a crash or a downtime gap without duplicating or shifting rows.
     *
     * @param timestamp timestamp to align.
     * @param bucket bucket width; a width of zero or less returns the timestamp unchanged.
     * @return start of the bucket containing timestamp.
     */
    [[maybe_unused]]
    static std::chrono::system_clock::time_point AlignDown(const std::chrono::system_clock::time_point &timestamp, const std::chrono::seconds bucket) {
        if (bucket <= std::chrono::seconds(0)) return timestamp;
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(timestamp.time_since_epoch()).count();
        return std::chrono::system_clock::time_point{std::chrono::seconds(seconds - seconds % bucket.count())};
    }

}// namespace Euclid::Database::Entity::Monitoring
