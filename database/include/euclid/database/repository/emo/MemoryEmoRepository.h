//
// Created by vogje01 on 8/18/26.
//
#pragma once

// C++ includes
#include <algorithm>
#include <map>
#include <mutex>
#include <ranges>
#include <tuple>
#include <vector>

// Euclid includes
#include <euclid/database/repository/emo/IEmoRepository.h>

namespace Euclid::Database {

    /**
     * @brief Monitoring data in-memory database.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class MemoryEmoRepository final : public IEmoRepository {

    public:

        /**
         * @brief Singleton instance
         */
        static MemoryEmoRepository &instance() {
            static MemoryEmoRepository monitoringDatabase;
            return monitoringDatabase;
        }

        /**
         * @brief Write one monitoring data point, replacing any existing point for the same bucket
         *
         * @param data monitoring data
         */
        void upsert(const Entity::Monitoring::MonitoringData &data) override {
            std::lock_guard lock(_mutex);
            upsertLocked(data);
        }

        /**
         * @brief Return a list of monitoring data
         *
         * @param query filter to apply
         * @return list of monitoring metrics
         */
        std::vector<Entity::Monitoring::MonitoringData> list(const MonitoringQuery &query) const override {
            std::lock_guard lock(_mutex);

            auto result = matching(query);
            std::ranges::sort(result, [](const auto &a, const auto &b) { return a.timestamp > b.timestamp; });

            if (query.limit > 0 && static_cast<long>(result.size()) > query.limit) {
                result.resize(static_cast<std::size_t>(query.limit));
            }
            return result;
        }

        /**
         * @brief Return a sample-weighted average over all labelName/labelValue
         *
         * @param query filter to apply
         * @return average value, or 0 if nothing matched
         */
        double average(const MonitoringQuery &query) const override {
            std::lock_guard lock(_mutex);

            double weighted = 0;
            long samples = 0;
            for (const auto &data: matching(query)) {
                weighted += data.value * static_cast<double>(data.samples);
                samples += data.samples;
            }
            return samples > 0 ? weighted / static_cast<double>(samples) : 0.0;
        }

        /**
         * @brief Aggregate one resolution tier into the next coarser one
         *
         * @param from tier to read
         * @param to tier to write
         * @param windowStart start of the source range, inclusive
         * @param windowEnd end of the source range, exclusive
         * @param retention how long the written buckets should be kept
         * @return number of buckets written
         */
        long rollup(const Entity::Monitoring::Resolution from, const Entity::Monitoring::Resolution to,
                    const std::chrono::system_clock::time_point windowStart, const std::chrono::system_clock::time_point windowEnd,
                    const std::chrono::seconds retention) override {
            std::lock_guard lock(_mutex);

            const auto bucketWidth = Entity::Monitoring::ResolutionBucket(to);

            // Same grouping the MongoDB $group stage performs: series plus target bucket.
            using BucketKey = std::tuple<std::string, std::string, std::string, std::chrono::system_clock::time_point>;
            std::map<BucketKey, Entity::Monitoring::MonitoringData> buckets;

            for (const auto &data: _store) {
                if (data.resolution != from) continue;
                if (data.timestamp < windowStart || data.timestamp >= windowEnd) continue;

                const auto bucket = Entity::Monitoring::AlignDown(data.timestamp, bucketWidth);
                const BucketKey key{data.name, data.labelName, data.labelValue, bucket};

                auto [it, inserted] = buckets.try_emplace(key);
                auto &target = it->second;
                if (inserted) {
                    target.name = data.name;
                    target.labelName = data.labelName;
                    target.labelValue = data.labelValue;
                    target.type = data.type;
                    target.resolution = to;
                    target.timestamp = bucket;
                    target.expiresAt = bucket + retention;
                    target.minValue = data.minValue;
                    target.maxValue = data.maxValue;
                } else {
                    target.minValue = std::min(target.minValue, data.minValue);
                    target.maxValue = std::max(target.maxValue, data.maxValue);
                }

                // value accumulates the total for a RATE and the sample-weighted sum for a GAUGE;
                // the GAUGE is divided back down by the sample count once the bucket is complete.
                target.value += data.type == Entity::Monitoring::MetricType::RATE ? data.value : data.value * static_cast<double>(data.samples);
                target.samples += data.samples;
            }

            for (auto &data: buckets | std::views::values) {
                if (data.type != Entity::Monitoring::MetricType::RATE) {
                    data.value = data.samples > 0 ? data.value / static_cast<double>(data.samples) : 0.0;
                }
                upsertLocked(data);
            }
            return static_cast<long>(buckets.size());
        }

        /**
         * @brief Clean up expired monitoring data
         *
         * @param now point in time to evaluate expiry against
         * @return number of data points deleted
         */
        long deleteExpired(const std::chrono::system_clock::time_point now) override {
            std::lock_guard lock(_mutex);
            return static_cast<long>(std::erase_if(_store, [&](const auto &data) { return data.expiresAt < now; }));
        }

    private:

        /**
         * @brief Replaces the data point for the same series and bucket, or appends it. Assumes the
         * lock is already held, so rollup() can write its results without releasing it.
         */
        void upsertLocked(const Entity::Monitoring::MonitoringData &data) {
            const auto existing = std::ranges::find_if(_store, [&](const auto &row) {
                return row.name == data.name && row.labelName == data.labelName && row.labelValue == data.labelValue &&
                       row.resolution == data.resolution && row.timestamp == data.timestamp;
            });
            if (existing != _store.end()) {
                *existing = data;
            } else {
                _store.push_back(data);
            }
        }

        /**
         * @brief Data points matching the query's filters, unsorted and unlimited. Assumes the lock
         * is already held.
         */
        [[nodiscard]]
        std::vector<Entity::Monitoring::MonitoringData> matching(const MonitoringQuery &query) const {

            const auto epoch = std::chrono::system_clock::time_point{};

            std::vector<Entity::Monitoring::MonitoringData> result;
            for (const auto &data: _store) {
                if (data.resolution != query.resolution) continue;
                if (!query.name.empty() && data.name != query.name) continue;
                if (!query.labelName.empty() && data.labelName != query.labelName) continue;
                if (!query.labelValue.empty() && data.labelValue != query.labelValue) continue;
                if (query.from != epoch && data.timestamp < query.from) continue;
                if (query.to != epoch && data.timestamp >= query.to) continue;
                result.push_back(data);
            }
            return result;
        }

        /**
         * @brief Monitoring data mutex
         */
        mutable std::mutex _mutex;

        /**
         * @brief memory database vector.
         */
        std::vector<Entity::Monitoring::MonitoringData> _store;

        // An in-memory store has no disk footprint to report, and inventing one from the map's
        // size would be a number nobody could act on.
        [[nodiscard]]
        std::optional<DatabaseStats> databaseStats() const override {
            return std::nullopt;
        }
    };

}// namespace Euclid::Database
