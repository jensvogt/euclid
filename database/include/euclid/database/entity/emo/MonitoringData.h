//
// Created by vogje01 on 8/18/26.
//

#pragma once

// C++ includes
#include <chrono>
#include <optional>
#include <string>

// MongoDB includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view-fwd.hpp>

// Euclid includes
#include <euclid/database/entity/emo/MetricType.h>
#include <euclid/database/entity/emo/Resolution.h>

namespace Euclid::Database::Entity::Monitoring {

    /**
     * @brief One aggregated metric data point, written periodically by the monitoring module.
     *
     * name/labelName/labelValue identify the metric series (e.g. name="sqs-service-time",
     * labelName="method", labelValue="send-message"); value is the aggregate over the bucket
     * starting at timestamp - see Core::Monitoring::MonitoringTimer/MetricEventBus for how a
     * module records the samples this is aggregated from.
     *
     * @par
     * name/labelName/labelValue/resolution/timestamp together identify a bucket uniquely, and are
     * the key every write upserts on. A data point therefore has no natural identity beyond its
     * series and its bucket, which is what lets a rollup be recomputed safely.
     */
    struct MonitoringData {

        /**
         * @brief ID
         */
        std::string oid;

        /**
         * @brief Metric name, e.g. "sqs-service-time", "sqs-service-count", "access-current-users".
         */
        std::string name;

        /**
         * @brief Label name, e.g. "method". Empty for unlabeled metrics.
         */
        std::string labelName;

        /**
         * @brief Label value, e.g. "send-message". Empty for unlabeled metrics.
         */
        std::string labelValue;

        /**
         * @brief Aggregate over the bucket: the total for a RATE, the mean for a GAUGE.
         */
        double value{};

        /**
         * @brief Smallest single sample seen in the bucket.
         *
         * Carried through the rollups because a mean alone becomes useless as resolution drops - a
         * daily average hides a three-minute spike completely, a daily maximum does not.
         */
        double minValue{};

        /**
         * @brief Largest single sample seen in the bucket.
         */
        double maxValue{};

        /**
         * @brief Number of underlying samples this point aggregates.
         *
         * Needed to roll a GAUGE up correctly: the mean of two bucket means is only the true mean
         * if both buckets carried the same number of samples, so the next tier weights by this.
         */
        long samples{};

        /**
         * @brief Whether value is a total (RATE) or a mean (GAUGE), deciding how a rollup combines it.
         */
        MetricType type{MetricType::GAUGE};

        /**
         * @brief Resolution tier this point belongs to.
         */
        Resolution resolution{Resolution::RAW};

        /**
         * @brief Start of the bucket this data point covers, floored to the resolution's bucket width.
         */
        std::chrono::system_clock::time_point timestamp;

        /**
         * @brief Point in time this data point may be deleted, derived from its resolution's
         * configured retention. Drives both the MongoDB TTL index and the monitoring module's
         * prune task.
         */
        std::chrono::system_clock::time_point expiresAt;

        /**
         * @brief Converts the entity to a MongoDB document
         *
         * @return entity as a MongoDB document.
         */
        [[nodiscard]]
        bsoncxx::document::value toDocument() const;

        /**
         * @brief Converts the MongoDB document to an entity
         *
         * @param doc MongoDB document.
         */
        static MonitoringData fromDocument(const std::optional<bsoncxx::document::view> &doc);
    };

}// namespace Euclid::Database::Entity::Monitoring
