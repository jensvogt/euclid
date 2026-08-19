#pragma once

// C++ includes
#include <chrono>
#include <string>
#include <utility>

// Euclid includes
#include <euclid/core/monitoring/MetricEventBus.h>

namespace Euclid::Core::Monitoring {

    /**
     * @brief RAII scoped timer - measures the wall-clock time between construction and
     * destruction and reports it as a gauge sample, optionally alongside a paired rate
     * (occurrence-count) sample.
     *
     * @par Usage
     * Instantiate as the first statement of the method being measured, e.g.
     * @code
     * Monitoring::MonitoringTimer measure("sqs-service-time", "sqs-service-count", "method", "send-message");
     * @endcode
     * Reports elapsed time under name "sqs-service-time" (labelName="method",
     * labelValue="send-message") and increments the paired counter "sqs-service-count" with the
     * same labels, however the method returns - including via an exception.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class MonitoringTimer {

    public:

        /**
         * @brief Times name only, no labels and no paired counter.
         */
        explicit MonitoringTimer(std::string name) : _timerName(std::move(name)), _start(std::chrono::steady_clock::now()) {}

        /**
         * @brief Times name with labels, no paired counter.
         */
        MonitoringTimer(std::string name, std::string labelName, std::string labelValue)
            : _timerName(std::move(name)), _labelName(std::move(labelName)), _labelValue(std::move(labelValue)),
              _start(std::chrono::steady_clock::now()) {}

        /**
         * @brief Times timerName and increments the paired counterName, both with the same labels.
         */
        MonitoringTimer(std::string timerName, std::string counterName, std::string labelName, std::string labelValue)
            : _timerName(std::move(timerName)), _counterName(std::move(counterName)), _labelName(std::move(labelName)),
              _labelValue(std::move(labelValue)), _start(std::chrono::steady_clock::now()) {}

        MonitoringTimer() = delete;

        MonitoringTimer(const MonitoringTimer &) = delete;

        MonitoringTimer &operator=(const MonitoringTimer &) = delete;

        ~MonitoringTimer() {
            const double elapsedMs = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - _start).count()) / 1000.0;
            if (!_counterName.empty()) MetricEventBus::instance().sigMetricRate(_counterName, _labelName, _labelValue);
            MetricEventBus::instance().sigMetricGauge(_timerName, _labelName, _labelValue, elapsedMs);
        }

    private:

        std::string _timerName;
        std::string _counterName;
        std::string _labelName;
        std::string _labelValue;
        std::chrono::steady_clock::time_point _start;
    };

}// namespace Euclid::Core::Monitoring