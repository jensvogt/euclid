#pragma once

// C++ includes
#include <string>

// Boost includes
#include <boost/signals2/signal.hpp>

namespace Euclid::Core::Monitoring {

    /**
     * @brief Process-local pub/sub hub for metric events.
     *
     * @par
     * Each euclid module runs as its own OS process, so - unlike a monolithic process - this hub
     * only ever sees the metrics recorded by whichever process it lives in. MonitoringCollector
     * subscribes to it to aggregate those metrics in-process; the monitoring module then polls
     * each process's aggregated snapshot over its own Unix socket (see
     * Core::HttpActionServer::MetricsResponse()) rather than crossing process boundaries here.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class MetricEventBus {

    public:

        /**
         * @brief Singleton instance
         */
        static MetricEventBus &instance() {
            static MetricEventBus bus;
            return bus;
        }

        MetricEventBus(const MetricEventBus &) = delete;
        MetricEventBus &operator=(const MetricEventBus &) = delete;

        /**
         * @brief Fired to record a gauge-style sample: name, labelName, labelValue, value.
         *
         * Aggregated as a mean across every sample recorded during a collection period - correct
         * both for a genuinely varying measurement (e.g. a timer's elapsed milliseconds) and for
         * a value that's identical across every call (e.g. a snapshot of shared state), since the
         * mean of N identical values is just that value.
         */
        boost::signals2::signal<void(std::string, std::string, std::string, double)> sigMetricGauge;

        /**
         * @brief Fired to record one occurrence of a rate-style event: name, labelName, labelValue.
         *
         * Aggregated as a count (occurrences since the last collection) - summed across every
         * process instance that reports it, since each process's count reflects only what
         * happened in that process.
         */
        boost::signals2::signal<void(std::string, std::string, std::string)> sigMetricRate;

        /**
         * @brief Fired to add an amount to a rate-style metric: name, labelName, labelValue, amount.
         *
         * The same metric sigMetricRate records, for a quantity that arrives in lumps rather than
         * one occurrence at a time - a transfer server counting the bytes of a file it just
         * stored, say. Aggregated as the sum of every amount recorded during a collection period,
         * which is what sigMetricRate produces too when each occurrence adds one.
         */
        boost::signals2::signal<void(std::string, std::string, std::string, double)> sigMetricCounter;

    private:

        MetricEventBus() = default;
    };

}// namespace Euclid::Core::Monitoring
