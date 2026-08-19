//
// Created by vogje01 on 8/18/26.
//
#pragma once

// C++ includes
#include <algorithm>
#include <mutex>
#include <vector>

// Euclid includes
#include <euclid/database/repository/monitoring/IMonitoringRepository.h>

namespace Euclid::Database {

    /**
     * @brief Monitoring data in-memory database.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class MemoryMonitoringRepository final : public IMonitoringRepository {

    public:

        /**
         * @brief Singleton instance
         */
        static MemoryMonitoringRepository &instance() {
            static MemoryMonitoringRepository monitoringDatabase;
            return monitoringDatabase;
        }

        void insert(const Entity::Monitoring::MonitoringData &data) override {
            std::lock_guard lock(_mutex);
            _store.push_back(data);
        }

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

        void deleteOlderThan(const std::chrono::system_clock::time_point cutoff) override {
            std::lock_guard lock(_mutex);
            std::erase_if(_store, [&](const auto &data) { return data.timestamp < cutoff; });
        }

    private:

        mutable std::mutex _mutex;
        std::vector<Entity::Monitoring::MonitoringData> _store;
    };

}// namespace Euclid::Database
