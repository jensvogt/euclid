//
// Created by vogje01 on 8/18/26.
//

#pragma once

// C++ includes
#include <chrono>
#include <string>
#include <vector>

// Euclid includes
#include <euclid/database/entity/emo/MonitoringData.h>

namespace Euclid::Database {

    /**
     * @brief Filter for monitoring data queries.
     *
     * Every string filter is optional and an empty one matches all values; an epoch-valued from/to
     * leaves that end of the time range open.
     */
    struct MonitoringQuery {

        /**
         * @brief Filter by metric name; empty matches all names.
         */
        std::string name;

        /**
         * @brief Filter by label name; empty matches all label names.
         */
        std::string labelName;

        /**
         * @brief Filter by label value; empty matches all label values.
         */
        std::string labelValue;

        /**
         * @brief Resolution tier to read from.
         */
        Entity::Monitoring::Resolution resolution{Entity::Monitoring::Resolution::RAW};

        /**
         * @brief Start of the time range, inclusive; epoch leaves it open.
         */
        std::chrono::system_clock::time_point from{};

        /**
         * @brief End of the time range, exclusive; epoch leaves it open.
         */
        std::chrono::system_clock::time_point to{};

        /**
         * @brief Maximum number of rows to return; 0 or less means no limit.
         */
        long limit{};
    };

    /**
     * @brief Interface for monitoring data repository operations.
     *
     * @par
     * Provides an abstraction for storing and querying the aggregated monitoring data points
     * written periodically by the monitoring module, across the three resolution tiers described
     * in Entity::Monitoring::Resolution.
     */
    class IEmoRepository {

    public:

        virtual ~IEmoRepository() = default;

        /**
         * @brief Writes one aggregated monitoring data point, replacing any existing point for the
         * same series and bucket.
         *
         * @par
         * Upsert rather than insert because both writers are idempotent by design: the flush task
         * writes a bucket-aligned timestamp, and a rollup recomputes whole buckets, including the
         * still-incomplete current one, on every run.
         *
         * @param data the data point to write; identified by name/labelName/labelValue/resolution/timestamp.
         */
        virtual void upsert(const Entity::Monitoring::MonitoringData &data) = 0;

        /**
         * @brief Lists monitoring data points, most recent first.
         *
         * @param query filter to apply.
         * @return matching data points, sorted by timestamp descending.
         */
        [[nodiscard]]
        virtual std::vector<Entity::Monitoring::MonitoringData> list(const MonitoringQuery &query) const = 0;

        /**
         * @brief Returns an average over all labelName/labelValue of one metric.
         *
         * @param query filter to apply; limit is ignored.
         * @return average value, or 0 if nothing matched.
         */
        [[nodiscard]]
        virtual double average(const MonitoringQuery &query) const = 0;

        /**
         * @brief Aggregates one resolution tier into the next coarser one.
         *
         * @par
         * Reads only the tier named by from, so a daily rollup touches the 24 hourly rows of a
         * series rather than its 288 raw ones. Buckets are recomputed rather than appended to, so
         * re-running an overlapping window is safe and repairs any gap left by downtime.
         *
         * @param from tier to read.
         * @param to tier to write; its bucket width decides the grouping.
         * @param windowStart start of the range of source rows to aggregate, inclusive.
         * @param windowEnd end of the range of source rows to aggregate, exclusive.
         * @param retention how long the written buckets should be kept, used to set their expiresAt.
         * @return number of buckets written.
         */
        virtual long rollup(Entity::Monitoring::Resolution from, Entity::Monitoring::Resolution to,
                            std::chrono::system_clock::time_point windowStart, std::chrono::system_clock::time_point windowEnd,
                            std::chrono::seconds retention) = 0;

        /**
         * @brief Deletes every data point whose expiresAt has passed.
         *
         * @param now point in time to evaluate expiry against.
         * @return number of data points deleted.
         */
        virtual long deleteExpired(std::chrono::system_clock::time_point now) = 0;
    };

}// namespace Euclid::Database
