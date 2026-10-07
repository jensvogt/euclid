// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE MonitoringCollectorTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <algorithm>
#include <optional>
#include <ranges>

// Euclid includes
#include <euclid/core/monitoring/MetricEventBus.h>
#include <euclid/core/monitoring/MonitoringCollector.h>

using Euclid::Core::Monitoring::MetricEventBus;
using Euclid::Core::Monitoring::MonitoringCollector;

// Three ways a metric reaches the collector, and what each one is worth at the end of a
// collection window: an occurrence counts one, an amount counts itself, and a gauge is averaged.
// The transfer servers rely on the middle one - bytes arrive in lumps of whatever a read
// returned, and what matters is their total over the window, not how many reads it took.

namespace {

    // Process-wide singleton: Collect() drains it, so each case starts from whatever the previous
    // one left behind only if it forgets to drain first.
    void drain() {
        std::ignore = MonitoringCollector::instance().Collect();
    }

    std::optional<MonitoringCollector::Sample> sampleOf(const std::vector<MonitoringCollector::Sample> &samples, const std::string &name) {
        const auto it = std::ranges::find(samples, name, &MonitoringCollector::Sample::name);
        if (it == samples.end()) return std::nullopt;
        return *it;
    }

    std::optional<MonitoringCollector::Sample> labelledSampleOf(const std::vector<MonitoringCollector::Sample> &samples, const std::string &name, const std::string &labelValue) {
        const auto it = std::ranges::find_if(samples, [&](const auto &s) { return s.name == name && s.labelValue == labelValue; });
        if (it == samples.end()) return std::nullopt;
        return *it;
    }

    struct StartedFixture {
        StartedFixture() { MonitoringCollector::instance().Start(); }
    };

}// namespace

BOOST_TEST_GLOBAL_FIXTURE(StartedFixture);

BOOST_AUTO_TEST_CASE(RateCountsOccurrences) {
    drain();

    auto &bus = MetricEventBus::instance();
    bus.sigMetricRate("ets-files-received", "server", "ftp-server");
    bus.sigMetricRate("ets-files-received", "server", "ftp-server");
    bus.sigMetricRate("ets-files-received", "server", "ftp-server");

    const auto sample = sampleOf(MonitoringCollector::instance().Collect(), "ets-files-received");
    BOOST_TEST_REQUIRE(sample.has_value());
    BOOST_TEST(sample->isRate);
    BOOST_TEST(sample->labelValue == "ftp-server");
    BOOST_TEST(sample->value == 3.0);
}

BOOST_AUTO_TEST_CASE(CounterSumsAmounts) {
    drain();

    auto &bus = MetricEventBus::instance();
    bus.sigMetricCounter("ets-bytes-received", "server", "ftp-server", 65536.0);
    bus.sigMetricCounter("ets-bytes-received", "server", "ftp-server", 1024.0);

    const auto sample = sampleOf(MonitoringCollector::instance().Collect(), "ets-bytes-received");
    BOOST_TEST_REQUIRE(sample.has_value());
    BOOST_TEST(sample->isRate);
    BOOST_TEST(sample->value == 66560.0);
}

BOOST_AUTO_TEST_CASE(LabelsSeparateServers) {
    drain();

    auto &bus = MetricEventBus::instance();
    bus.sigMetricCounter("ets-bytes-sent", "server", "ftp-server", 100.0);
    bus.sigMetricCounter("ets-bytes-sent", "server", "sftp-server", 400.0);

    const auto samples = MonitoringCollector::instance().Collect();
    BOOST_TEST(std::ranges::count(samples, std::string("ets-bytes-sent"), &MonitoringCollector::Sample::name) == 2);

    const auto ftp = labelledSampleOf(samples, "ets-bytes-sent", "ftp-server");
    const auto sftp = labelledSampleOf(samples, "ets-bytes-sent", "sftp-server");
    BOOST_TEST_REQUIRE(ftp.has_value());
    BOOST_TEST_REQUIRE(sftp.has_value());
    BOOST_TEST(ftp->value == 100.0);
    BOOST_TEST(sftp->value == 400.0);
}

BOOST_AUTO_TEST_CASE(EveryDimensionSeparatesASeries) {
    drain();

    // A load average needs two: the host says whose it is, the window says which of the three the
    // kernel reports. Keyed by the single pair alone these collapsed into one entry whose value
    // was the mean of all three - a number that is not any of the figures measured.
    auto &bus = MetricEventBus::instance();
    bus.sigMetricGaugeWithLabels("system-load-average", {{"host", "pi"}, {"interval", "1m"}}, 4.0);
    bus.sigMetricGaugeWithLabels("system-load-average", {{"host", "pi"}, {"interval", "5m"}}, 2.0);
    bus.sigMetricGaugeWithLabels("system-load-average", {{"host", "pi"}, {"interval", "15m"}}, 1.0);

    // And the host is a dimension like any other, so two machines are two series per window.
    bus.sigMetricGaugeWithLabels("system-load-average", {{"host", "desktop"}, {"interval", "1m"}}, 8.0);

    const auto samples = MonitoringCollector::instance().Collect();
    BOOST_TEST(std::ranges::count(samples, std::string("system-load-average"), &MonitoringCollector::Sample::name) == 4);

    const auto of = [&samples](const std::string &host, const std::string &interval) -> std::optional<double> {
        const auto it = std::ranges::find_if(samples, [&](const auto &s) {
            return s.name == "system-load-average" && s.labels.contains("host") && s.labels.at("host") == host
                   && s.labels.contains("interval") && s.labels.at("interval") == interval;
        });
        return it == samples.end() ? std::nullopt : std::optional{it->value};
    };

    BOOST_TEST(of("pi", "1m").value_or(-1) == 4.0);
    BOOST_TEST(of("pi", "5m").value_or(-1) == 2.0);
    BOOST_TEST(of("pi", "15m").value_or(-1) == 1.0);
    BOOST_TEST(of("desktop", "1m").value_or(-1) == 8.0);
}

BOOST_AUTO_TEST_CASE(ADimensionedGaugeIsStillAveraged) {
    drain();

    // The same rule as a single-pair gauge: the mean over the window, not the last value.
    auto &bus = MetricEventBus::instance();
    bus.sigMetricGaugeWithLabels("system-cpu-usage", {{"host", "pi"}}, 20.0);
    bus.sigMetricGaugeWithLabels("system-cpu-usage", {{"host", "pi"}}, 40.0);

    const auto samples = MonitoringCollector::instance().Collect();
    const auto sample = sampleOf(samples, "system-cpu-usage");
    BOOST_TEST_REQUIRE(sample.has_value());
    BOOST_TEST(sample->value == 30.0);
    BOOST_TEST(!sample->isRate);
}

BOOST_AUTO_TEST_CASE(GaugeAveragesSamples) {
    drain();

    auto &bus = MetricEventBus::instance();
    bus.sigMetricGauge("ets-service-time", "method", "list-servers", 10.0);
    bus.sigMetricGauge("ets-service-time", "method", "list-servers", 20.0);

    const auto sample = sampleOf(MonitoringCollector::instance().Collect(), "ets-service-time");
    BOOST_TEST_REQUIRE(sample.has_value());
    BOOST_TEST(!sample->isRate);
    BOOST_TEST(sample->value == 15.0);
}

BOOST_AUTO_TEST_CASE(CollectResetsTheWindow) {
    drain();

    MetricEventBus::instance().sigMetricCounter("ets-bytes-received", "server", "ftp-server", 42.0);
    BOOST_TEST_REQUIRE(sampleOf(MonitoringCollector::instance().Collect(), "ets-bytes-received").has_value());

    // Nothing recorded since, so the next window reports nothing rather than the same total again.
    BOOST_TEST(!sampleOf(MonitoringCollector::instance().Collect(), "ets-bytes-received").has_value());
}
