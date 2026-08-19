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

    std::string MonitoringCollector::key(const std::string &name, const std::string &labelName, const std::string &labelValue) {
        return name + ":" + labelName + ":" + labelValue;
    }

    void MonitoringCollector::Start() {
        std::lock_guard lock(_mutex);
        if (_started) return;
        _started = true;

        auto &bus = MetricEventBus::instance();
        bus.sigMetricGauge.connect([this](const std::string &name, const std::string &labelName, const std::string &labelValue, const double value) {
            setGauge(name, labelName, labelValue, value);
        });
        bus.sigMetricRate.connect([this](const std::string &name, const std::string &labelName, const std::string &labelValue) {
            increment(name, labelName, labelValue);
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

    void MonitoringCollector::increment(const std::string &name, const std::string &labelName, const std::string &labelValue) {
        std::lock_guard lock(_mutex);
        auto &entry = _entries[key(name, labelName, labelValue)];
        entry.name = name;
        entry.labelName = labelName;
        entry.labelValue = labelValue;
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
                               .value = entry.isRate ? static_cast<double>(entry.count) : entry.sum / static_cast<double>(entry.count),
                               .isRate = entry.isRate});
        }
        return result;
    }

}// namespace Euclid::Core::Monitoring
