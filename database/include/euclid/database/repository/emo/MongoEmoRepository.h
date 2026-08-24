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
         * @brief Singleton instance
         */
        static MongoEmoRepository &instance() {
            static MongoEmoRepository monitoringDatabase;
            return monitoringDatabase;
        }

        void insert(const Entity::Monitoring::MonitoringData &data) override;

        [[nodiscard]]
        std::vector<Entity::Monitoring::MonitoringData> list(const std::string &name, const std::string &labelName, const std::string &labelValue, long limit) const override;

        void deleteOlderThan(std::chrono::system_clock::time_point cutoff) override;

    private:

        static constexpr auto COLLECTION = "monitoring_data";
    };

}// namespace Euclid::Database
