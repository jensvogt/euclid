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
         * @brief Insert new monitoring data
         *
         * @param data monitoring data
         */
        void insert(const Entity::Monitoring::MonitoringData &data) override;

        /**
         * @brief Return a list of monitoring data
         *
         * @param name metric name
         * @param labelName label name
         * @param labelValue label value
         * @param limit maximal number of monitoring data entries
         * @return list of monitoring metrics
         */
        [[nodiscard]]
        std::vector<Entity::Monitoring::MonitoringData> list(const std::string &name, const std::string &labelName, const std::string &labelValue, long limit) const override;

        /**
         * @brief Returns an average over all labelName/labelvalue
         *
         * @param name       filter by metric name; empty matches all names.
         * @return matching data points, sorted by timestamp descending.
         */
        [[nodiscard]]
        double average(const std::string &name) const override;

        /**
         * @brief clean up monitoring data
         *
         * @param cutoff cut off timestamp
         */
        void deleteOlderThan(std::chrono::system_clock::time_point cutoff) override;

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