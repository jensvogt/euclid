//
// Created by vogje01 on 8/18/26.
//

#pragma once

// C++ includes
#include <chrono>
#include <string>
#include <vector>

// Euclid includes
#include <euclid/database/entity/monitoring/MonitoringData.h>

namespace Euclid::Database {

    /**
     * @brief Interface for monitoring data repository operations.
     *
     * Provides an abstraction for storing and querying the averaged monitoring data points
     * written periodically by the monitoring module.
     */
    class IEmoRepository {

    public:

        virtual ~IEmoRepository() = default;

        /**
         * @brief Inserts one averaged monitoring data point.
         *
         * @param data the data point to insert.
         */
        virtual void insert(const Entity::Monitoring::MonitoringData &data) = 0;

        /**
         * @brief Lists monitoring data points, most recent first.
         *
         * @param name       filter by metric name; empty matches all names.
         * @param labelName  filter by label name; empty matches all label names.
         * @param labelValue filter by label value; empty matches all label values.
         * @param limit      maximum number of rows to return; 0 or less means no limit.
         * @return matching data points, sorted by timestamp descending.
         */
        [[nodiscard]]
        virtual std::vector<Entity::Monitoring::MonitoringData> list(const std::string &name, const std::string &labelName, const std::string &labelValue, long limit) const = 0;

        /**
         * @brief Deletes every data point whose timestamp is older than cutoff.
         *
         * @param cutoff data points with timestamp before this point in time are removed.
         */
        virtual void deleteOlderThan(std::chrono::system_clock::time_point cutoff) = 0;
    };

}// namespace Euclid::Database
