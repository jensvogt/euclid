// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <ranges>

// Euclid includes
#include <euclid/core/monitoring/MetricEventBus.h>
#include <euclid/core/monitoring/MonitoringCollector.h>

namespace Euclid::Core::Monitoring {

    MonitoringCollector &MonitoringCollector::instance() {
        static MonitoringCollector collector;
        return collector;
    }

    std::string MonitoringCollector::key(const std::string &name, const std::string &labelName, const std::string &labelValue,
                                         const std::map<std::string, std::string> &labels) {

        // std::map iterates in key order, so the same dimensions always produce the same key
        // whatever order the caller named them in.
        std::string composed = name + ":" + labelName + ":" + labelValue;
        for (const auto &[label, value]: labels) composed += ":" + label + "=" + value;
        return composed;
    }

    void MonitoringCollector::Start() {
        std::lock_guard lock(_mutex);
        if (_started) return;
        _started = true;

        auto &bus = MetricEventBus::instance();
        bus.sigMetricGauge.connect([this](const std::string &name, const std::string &labelName, const std::string &labelValue, const double value) {
            setGauge(name, labelName, labelValue, value);
        });
        bus.sigMetricGaugeWithLabels.connect([this](const std::string &name, const std::map<std::string, std::string> &labels, const double value) {
            setGauge(name, labels, value);
        });
        bus.sigMetricRate.connect([this](const std::string &name, const std::string &labelName, const std::string &labelValue) {
            increment(name, labelName, labelValue);
        });
        bus.sigMetricCounter.connect([this](const std::string &name, const std::string &labelName, const std::string &labelValue, const double amount) {
            increment(name, labelName, labelValue, amount);
        });
    }

    void MonitoringCollector::setGauge(const std::string &name, const std::string &labelName, const std::string &labelValue, const double value) {
        std::lock_guard lock(_mutex);
        auto &entry = _entries[key(name, labelName, labelValue)];
        entry.name = name;
        entry.labelName = labelName;
        entry.labelValue = labelValue;
        entry.sum += value;
        entry.count++;
        entry.isRate = false;
    }

    void MonitoringCollector::setGauge(const std::string &name, const std::map<std::string, std::string> &labels, const double value) {
        std::lock_guard lock(_mutex);
        auto &entry = _entries[key(name, {}, {}, labels)];
        entry.name = name;
        entry.labels = labels;
        entry.sum += value;
        entry.count++;
        entry.isRate = false;
    }

    // A plain occurrence is an amount of one, so counting occurrences and summing amounts are the
    // same accumulation - which is why both live in sum, and why Collect() reports it for every
    // rate metric regardless of how it was recorded.
    void MonitoringCollector::increment(const std::string &name, const std::string &labelName, const std::string &labelValue, const double amount) {
        std::lock_guard lock(_mutex);
        auto &entry = _entries[key(name, labelName, labelValue)];
        entry.name = name;
        entry.labelName = labelName;
        entry.labelValue = labelValue;
        entry.sum += amount;
        entry.count++;
        entry.isRate = true;
    }

    std::vector<MonitoringCollector::Sample> MonitoringCollector::Collect() {
        std::map<std::string, Entry> snapshot;
        {
            std::lock_guard lock(_mutex);
            snapshot.swap(_entries);
        }

        std::vector<Sample> result;
        result.reserve(snapshot.size());
        for (const auto &entry: snapshot | std::views::values) {
            if (entry.count == 0) continue;
            result.push_back({.name = entry.name,
                               .labelName = entry.labelName,
                               .labelValue = entry.labelValue,
                               .labels = entry.labels,
                               .value = entry.isRate ? entry.sum : entry.sum / static_cast<double>(entry.count),
                               .isRate = entry.isRate});
        }
        return result;
    }

}// namespace Euclid::Core::Monitoring
