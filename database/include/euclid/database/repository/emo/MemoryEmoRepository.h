//
// Created by vogje01 on 8/18/26.
//
#pragma once

// C++ includes
#include <algorithm>
#include <mutex>
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
         * @brief Insert new monitoring data
         *
         * @param data monitoring data
         */
        void insert(const Entity::Monitoring::MonitoringData &data) override {
            std::lock_guard lock(_mutex);
            _store.push_back(data);
        }

        /**
         * @brief Return a list of monitoring data
         *
         * @param name metric name
         * @param labelName label name
         * @param labelValue label value
         * @param limit maximal number of monitoring data entries
         * @return list of monitoring metrics
         */
        std::vector<Entity::Monitoring::MonitoringData> list(const std::string &name, const std::string &labelName, const std::string &labelValue, const long limit) const override {
            std::lock_guard lock(_mutex);

            std::vector<Entity::Monitoring::MonitoringData> result;
            for (const auto &data: _store) {
                if (!name.empty() && data.name != name) continue;
                if (!labelName.empty() && data.labelName != labelName) continue;
                if (!labelValue.empty() && data.labelValue != labelValue) continue;
                result.push_back(data);
            }

            std::ranges::sort(result, [](const auto &a, const auto &b) { return a.timestamp > b.timestamp; });

            if (limit > 0 && static_cast<long>(result.size()) > limit) {
                result.resize(static_cast<std::size_t>(limit));
            }
            return result;
        }

        /**
         * @brief Return an average over all labelName/labelValue
         *
         * @param name metric name
         * @return list of monitoring metrics
         */
        double average(const std::string &name) const override {
            std::lock_guard lock(_mutex);

            std::vector<Entity::Monitoring::MonitoringData> result;
            for (const auto &data: _store) {
                if (!name.empty() && data.name != name) continue;
                result.push_back(data);
            }

            return 0.0;
        }

        /**
         * @brief clean up monitoring data
         *
         * @param cutoff cut off timestamp
         */
        void deleteOlderThan(const std::chrono::system_clock::time_point cutoff) override {
            std::lock_guard lock(_mutex);
            std::erase_if(_store, [&](const auto &data) { return data.timestamp < cutoff; });
        }

    private:

        /**
         * @brief Monitoring data mutex
         */
        mutable std::mutex _mutex;

        /**
         * @brief memory database vector.
         */
        std::vector<Entity::Monitoring::MonitoringData> _store;
    };

}// namespace Euclid::Database