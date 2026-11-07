// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <chrono>
#include <string>

namespace Euclid::Core::Monitoring {

    /**
     * @brief How often a process samples what it is costing. One minute, as every module has.
     */
    constexpr auto kProcessUsagePeriod = std::chrono::seconds(60);

    /**
     * @brief Records what this process is costing the machine, under @p serviceName.
     *
     * @par
     * Four gauges, all labelled "module" with the name given: "euclid-cpu-usage" is this process's
     * own share of the machine's CPU since the previous call, and "euclid-memory-usage-real-mb",
     * "-virtual-mb" and "-percent" are what it is holding now. Picked up by MonitoringCollector and
     * pushed to the monitoring module by MetricsPusher, like any other MetricEventBus metric.
     *
     * @par
     * The process's share, not the host's - the machine's own figures are "system-cpu-usage" and
     * "system-memory-usage-percent", which EMO records. On the same scale as those: 100 is the whole
     * machine rather than one core.
     *
     * @par
     * Here rather than in HttpActionServer, which sampled this for every module, because the manager
     * is not built on one: it runs the gateway and nothing was recording what that process costs. One
     * implementation, because the CPU figure is a delta between calls and two of them in a process
     * would each see half the samples.
     *
     * @param serviceName what the "module" label says - a module's name, or "gateway" for the manager.
     */
    void RecordProcessUsage(const std::string &serviceName);

}// namespace Euclid::Core::Monitoring
