// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/18/26.
//

#pragma once

// C++ includes
#include <chrono>
#include <map>
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
     * name and labels identify the metric series (e.g. name="sqs-service-time",
     * labels={"method": "send-message"}); value is the aggregate over the bucket starting at
     * timestamp - see Core::Monitoring::MonitoringTimer/MetricEventBus for how a module records
     * the samples this is aggregated from.
     *
     * @par Why a map rather than the one label pair this used to carry
     * euclid's own metrics are one-dimensional - a service time by method, an instance count by
     * module - and a single labelName/labelValue said that perfectly well. Metrics pushed by an
     * application are not: a Micrometer or Prometheus meter carries however many dimensions the
     * application gave it ({"area": "heap", "id": "G1 Eden Space"}), and flattening those into one
     * pair means either losing dimensions or inventing a composite value nothing can aggregate
     * across. Rows written before this have no "labels" field and are read as a one-entry map, so
     * nothing already stored is lost or has to be migrated.
     *
     * @par Identity
     * name/labelKey/resolution/timestamp together identify a bucket uniquely, and are the key
     * every write upserts on. labelKey is the canonical rendering of the map (see LabelKey), used
     * rather than the map itself because BSON compares subdocuments field by field in stored
     * order - two rows with the same labels written in a different order would be two series - and
     * because $merge's "on" needs fields it can put a unique index on. A data point therefore has
     * no natural identity beyond its series and its bucket, which is what lets a rollup be
     * recomputed safely.
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
         * @brief The series' dimensions, e.g. {"method": "send-message"}. Empty for an unlabeled
         * metric. Ordered, because std::map is - which is what makes LabelKey() canonical.
         */
        std::map<std::string, std::string> labels;

        /**
         * @brief The first label's name, or empty when there are none.
         *
         * For the readers that predate the map and only ever deal in one-dimensional series - a
         * listing that shows "which method", a graph drawn per label value. A caller that cares
         * about a specific dimension should read "labels" by name instead of trusting the order.
         */
        [[nodiscard]]
        std::string labelName() const { return labels.empty() ? std::string{} : labels.begin()->first; }

        /**
         * @brief The first label's value, or empty when there are none. See labelName().
         */
        [[nodiscard]]
        std::string labelValue() const { return labels.empty() ? std::string{} : labels.begin()->second; }

        /**
         * @brief The canonical rendering of a label map: "k1=v1;k2=v2", keys in order.
         *
         * Static and taking the map rather than reading the member, because both the writers and
         * the queries need to produce it for labels they are holding but have no row for.
         */
        static std::string LabelKey(const std::map<std::string, std::string> &labels) {
            std::string key;
            for (const auto &[name, value]: labels) {
                if (!key.empty()) key += ';';
                key += name;
                key += '=';
                key += value;
            }
            return key;
        }

        /**
         * @brief This row's label key.
         */
        [[nodiscard]]
        std::string labelKey() const { return LabelKey(labels); }

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
