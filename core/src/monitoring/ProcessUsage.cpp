// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <mutex>
#include <optional>

// Euclid includes
#include <euclid/core/SystemUtils.h>
#include <euclid/core/monitoring/MetricEventBus.h>
#include <euclid/core/monitoring/ProcessUsage.h>

namespace Euclid::Core::Monitoring {

    namespace {

        // The previous reading of this process's CPU time and the machine's, taken together, so
        // recordCpuUsage() can compute a delta-based percentage rather than a since-boot average.
        // One per process, so file-local state is simplest - and it has to be one, since the figure
        // is a delta between calls: two samplers would each see half the samples and report half the
        // CPU. That is the reason this lives here rather than being written out twice.
        struct CpuReading {
            unsigned long long processTime = 0;
            SystemUtils::CpuTimes systemTimes;
        };

        std::mutex &cpuTimesMutex() {
            static std::mutex mutex;
            return mutex;
        }

        std::optional<CpuReading> &previousCpuReading() {
            static std::optional<CpuReading> previous;
            return previous;
        }

        // Records this process's own share of the machine's CPU since the previous call as a
        // "euclid-cpu-usage" gauge, labelled with the given serviceName - picked up by
        // MonitoringCollector and pushed to the monitoring module by MetricsPusher, same as any
        // other MetricEventBus metric. The first call after process start only primes the
        // previous-reading state, since a delta needs two samples.
        //
        // The process's share, not the host's. This used to report the machine's whole CPU usage
        // under every module's label, so every module drew the same line and none of them said
        // what the module itself cost - the host's figure is "system-cpu-usage", recorded by EMO.
        // On the same scale as that one: 100 is the whole machine, not one core.
        void recordCpuUsage(const std::string &serviceName) {

            const auto processTime = SystemUtils::ReadProcessCpuTime();
            const auto systemTimes = SystemUtils::ReadCpuTimes();
            if (!processTime.has_value() || !systemTimes.has_value()) return;

            std::lock_guard lock(cpuTimesMutex());
            const auto previous = previousCpuReading();
            previousCpuReading() = CpuReading{.processTime = *processTime, .systemTimes = *systemTimes};
            if (!previous.has_value()) return;

            if (const auto percent = SystemUtils::ProcessCpuUsagePercent(previous->processTime, *processTime,
                                                                         previous->systemTimes, *systemTimes);
                percent.has_value()) {
                MetricEventBus::instance().sigMetricGauge("euclid-cpu-usage", "module", serviceName, *percent);
            }
        }

        // Records this process's current memory usage as three gauges, all labelled with this
        // module's serviceName - real/virtual memory in MB, plus real memory as a percentage of
        // the system's total RAM. Unlike recordCpuUsage(), this is a direct point-in-time
        // reading, so there's no state to prime on the first call.
        void recordMemoryUsage(const std::string &serviceName) {

            const auto usage = SystemUtils::ReadMemoryUsage();
            if (!usage.has_value()) return;

            auto &bus = MetricEventBus::instance();
            bus.sigMetricGauge("euclid-memory-usage-real-mb", "module", serviceName, usage->realMb);
            bus.sigMetricGauge("euclid-memory-usage-virtual-mb", "module", serviceName, usage->virtualMb);
            bus.sigMetricGauge("euclid-memory-usage-percent", "module", serviceName, usage->percentOfTotal);
        }

    }// namespace

    void RecordProcessUsage(const std::string &serviceName) {
        recordCpuUsage(serviceName);
        recordMemoryUsage(serviceName);
    }

}// namespace Euclid::Core::Monitoring
