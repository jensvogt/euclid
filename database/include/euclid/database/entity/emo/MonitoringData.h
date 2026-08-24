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

namespace Euclid::Database::Entity::Monitoring {

    /**
     * @brief One averaged metric data point, written periodically by the monitoring module.
     *
     * name/labelName/labelValue identify the metric series (e.g. name="sqs-service-time",
     * labelName="method", labelValue="send-message"); value is the average over the collection
     * window ending at timestamp - see Core::Monitoring::MonitoringTimer/MetricEventBus for how a
     * module records the samples this is averaged from.
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
         * @brief Average value over the collection window ending at timestamp.
         */
        double value{};

        /**
         * @brief End of the collection window this data point covers.
         */
        std::chrono::system_clock::time_point timestamp;

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
