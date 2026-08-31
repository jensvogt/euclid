//
// Created by vogje01 on 8/18/26.
//

#pragma once

// Euclid includes
#include <euclid/core/LogStream.h>
#include <euclid/database/Database.h>
#include <euclid/database/repository/emo/IEmoRepository.h>

namespace Euclid::Database {

    /**
     * @brief Monitoring data MongoDB database.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class MongoEmoRepository final : public IEmoRepository {

    public:

        /**
         * @brief Constructor
         */
        explicit MongoEmoRepository();

        /**
         * @brief Singleton instance
         */
        static MongoEmoRepository &instance() {
            static MongoEmoRepository monitoringDatabase;
            return monitoringDatabase;
        }

        /**
         * @brief Write one monitoring data point, replacing any existing point for the same bucket
         *
         * @param data monitoring data
         */
        void upsert(const Entity::Monitoring::MonitoringData &data) override;

        /**
         * @brief Return a list of monitoring data
         *
         * @param query filter to apply
         * @return list of monitoring metrics
         */
        [[nodiscard]]
        std::vector<Entity::Monitoring::MonitoringData> list(const MonitoringQuery &query) const override;

        /**
         * @brief Returns an average over all labelName/labelValue
         *
         * @param query filter to apply
         * @return average value
         */
        [[nodiscard]]
        double average(const MonitoringQuery &query) const override;

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
        long rollup(Entity::Monitoring::Resolution from, Entity::Monitoring::Resolution to,
                    std::chrono::system_clock::time_point windowStart, std::chrono::system_clock::time_point windowEnd,
                    std::chrono::seconds retention) override;

        /**
         * @brief Clean up expired monitoring data
         *
         * @param now point in time to evaluate expiry against
         * @return number of data points deleted
         */
        long deleteExpired(std::chrono::system_clock::time_point now) override;

    private:

        /**
         * @brief Collection name
         */
        static constexpr auto COLLECTION = "emo_data";

        /**
         * @brief Creates the indexes required for efficient monitoring data queries, if they do not already exist.
         */
        static void ensureIndexes();
    };

}// namespace Euclid::Database
