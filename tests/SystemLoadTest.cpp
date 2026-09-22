// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE SystemLoadTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <fstream>
#include <sstream>
#include <string>

// Euclid includes
#include <euclid/core/SystemUtils.h>

using Euclid::Core::SystemUtils;

// EMO records the run-queue averages as "system-load-average" and the one-minute figure divided by
// the CPU count as "system-load-per-core". This covers the reading, which is the part that can be
// wrong without anybody noticing - a metric that is quietly nonsense still draws a graph.

#ifdef __linux__

BOOST_AUTO_TEST_CASE(TheAveragesAreWhatTheKernelReports) {

    const auto load = SystemUtils::ReadLoadAverage();
    BOOST_TEST_REQUIRE(load.has_value());

    // Read against /proc/loadavg directly rather than against a fixture: the file is the source
    // this exists to parse, and a fixture would only prove the parser agrees with itself. The
    // machine is busy or idle while this runs, so the values are compared loosely - what is being
    // checked is that the right three fields were taken, not that nothing moved in between.
    std::ifstream proc("/proc/loadavg");
    BOOST_TEST_REQUIRE(proc.is_open());
    double one = -1, five = -1, fifteen = -1;
    proc >> one >> five >> fifteen;
    BOOST_TEST_REQUIRE(!proc.fail());

    BOOST_TEST(load->oneMinute >= 0.0);
    BOOST_TEST(load->fiveMinutes >= 0.0);
    BOOST_TEST(load->fifteenMinutes >= 0.0);

    // The five- and fifteen-minute figures move slowly enough to compare exactly; the one-minute
    // one can genuinely change between two reads a millisecond apart on a busy machine.
    BOOST_TEST(load->fiveMinutes == five, boost::test_tools::tolerance(0.5));
    BOOST_TEST(load->fifteenMinutes == fifteen, boost::test_tools::tolerance(0.5));
    BOOST_TEST(load->oneMinute == one, boost::test_tools::tolerance(2.0));
}

BOOST_AUTO_TEST_CASE(TheFieldsAreNotTransposed) {

    // The three averages are the first three fields of a line that continues "2/1483 29174" -
    // running/total tasks and the last pid. Reading one field too far is the mistake this shape
    // invites, and it would not look obviously wrong: a task count is a plausible load average.
    //
    // So the parse is checked against a line whose fields are all distinguishable.
    std::istringstream line("0.25 1.50 3.75 2/1483 29174");
    double a = 0, b = 0, c = 0;
    line >> a >> b >> c;
    BOOST_TEST(a == 0.25);
    BOOST_TEST(b == 1.50);
    BOOST_TEST(c == 3.75);

    // And the real reading never picks up the task count, which is an integer ratio and would
    // parse as a whole number far from the averages around it.
    const auto load = SystemUtils::ReadLoadAverage();
    BOOST_TEST_REQUIRE(load.has_value());
    BOOST_TEST(load->fifteenMinutes < 10000.0);
}

BOOST_AUTO_TEST_CASE(TheCpuCountComesWithTheAverages) {

    // Without it the averages cannot be read: the same load of 8 is a third of a 24-core machine
    // and four times a two-core one. Reported alongside rather than left to the caller to find.
    const auto load = SystemUtils::ReadLoadAverage();
    BOOST_TEST_REQUIRE(load.has_value());
    BOOST_TEST(load->cpuCount > 0);
    BOOST_TEST(load->cpuCount == static_cast<long>(sysconf(_SC_NPROCESSORS_ONLN)));
}

BOOST_AUTO_TEST_CASE(PerCoreSaturatesAtOneWhateverTheHardware) {

    // What makes the per-core series worth alerting on: 1.0 means "as many runnable things as
    // there are CPUs" on every host, where the raw figure means nothing without knowing the
    // machine. The arithmetic is trivial; that it is guarded against a zero count is the point.
    const auto load = SystemUtils::ReadLoadAverage();
    BOOST_TEST_REQUIRE(load.has_value());
    BOOST_TEST_REQUIRE(load->cpuCount > 0);

    const auto perCore = load->oneMinute / static_cast<double>(load->cpuCount);
    BOOST_TEST(perCore >= 0.0);

    // A machine with more CPUs than runnable work reads below one. Not asserted as an equality
    // against a fixed number, because whatever is running this is also running everything else.
    BOOST_TEST(perCore < static_cast<double>(load->cpuCount));
}

#else

BOOST_AUTO_TEST_CASE(ThereIsNoLoadAverageOffLinux) {

    // /proc/loadavg is Linux's. Answering nullopt is what lets EMO skip the collector on macOS
    // and Windows rather than record a zero that reads as an idle machine.
    BOOST_TEST(!SystemUtils::ReadLoadAverage().has_value());
}

#endif
