#pragma once

// C++ includes
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace Euclid::Core::Monitoring {

    /**
     * @brief Process-local in-memory aggregator for metric events.
     *
     * Subscribes to MetricEventBus (once Start() is called) and keeps a running sum/count per
     * "name:labelName:labelValue" key. Collect() returns the aggregate since the last call and
     * resets it, ready for the next collection window - the monitoring module drives when that
     * happens, by polling Core::HttpActionServer::MetricsResponse() periodically.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class MonitoringCollector {

    public:

        /**
         * @brief Singleton instance
         */
        static MonitoringCollector &instance();

        /**
         * @brief Subscribes to MetricEventBus. Idempotent - safe to call more than once.
         */
        void Start();

        /**
         * @brief One aggregated metric, ready to report or persist.
         */
        struct Sample {
            std::string name;
            std::string labelName;
            std::string labelValue;

            /**
             * @brief For a rate metric, the number of occurrences since the last Collect(). For a
             * gauge metric, the mean of every sample recorded since the last Collect().
             */
            double value{};

            /**
             * @brief True if this is a rate (occurrence-count) metric, false if it's a gauge
             * (averaged-value) metric - determines how the monitoring module aggregates this
             * sample further, across instances and over its own averaging period.
             */
            bool isRate{};
        };

        /**
         * @brief Returns every metric accumulated since the last call, and resets the
         * accumulators for the next collection window.
         *
         * @return the accumulated samples; empty if nothing was recorded since the last call.
         */
        [[nodiscard]]
        std::vector<Sample> Collect();

    private:

        MonitoringCollector() = default;

        void setGauge(const std::string &name, const std::string &labelName, const std::string &labelValue, double value);

        void increment(const std::string &name, const std::string &labelName, const std::string &labelValue);

        static std::string key(const std::string &name, const std::string &labelName, const std::string &labelValue);

        struct Entry {
            std::string name;
            std::string labelName;
            std::string labelValue;
            double sum = 0;
            long count = 0;
            bool isRate = false;
        };

        std::mutex _mutex;
        std::map<std::string, Entry> _entries;
        bool _started = false;
    };

}// namespace Euclid::Core::Monitoring
