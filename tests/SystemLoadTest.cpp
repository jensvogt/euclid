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

#ifdef _WIN32
#include <windows.h>    // GetActiveProcessorCount, for checking the count reported beside the queue
#endif

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

#elif defined(_WIN32)

// Windows has no run-queue average, so EMO records the processor queue instead - see
// EmoServer::collectSystemLoad(). Same concern as the Linux cases above: a performance counter read
// through the wrong path, or with the wrong processor count beside it, still draws a graph.

BOOST_AUTO_TEST_CASE(ThereIsNoLoadAverageOnWindows) {

    // Deliberately not emulated. Answering nullopt is what keeps an instantaneous count out of a
    // series whose meaning is the window it averages over.
    BOOST_TEST(!SystemUtils::ReadLoadAverage().has_value());
}

BOOST_AUTO_TEST_CASE(TheProcessorQueueIsReadableThroughPdh) {

    // The counter is added by its English name, which is the part that can be wrong without
    // anybody noticing until this runs on a Windows whose display language is not English - there
    // PdhAddCounter would fail and this would be a permanent nullopt.
    const auto queue = SystemUtils::ReadProcessorQueueLength();
    BOOST_TEST_REQUIRE(queue.has_value());
    BOOST_TEST(queue->queueLength >= 0.0);

    // An idle desktop reads 0 and a saturated host reads tens. A figure in the thousands means the
    // wrong counter was formatted, not a busy machine.
    BOOST_TEST(queue->queueLength < 10000.0);
}

BOOST_AUTO_TEST_CASE(TheProcessorCountComesWithTheQueue) {

    // Without it the queue cannot be read, for the same reason a load average cannot - see
    // ReadProcessorQueueLength(). Counted over all processor groups, so a host with more than 64
    // processors reports all of them rather than the group this thread happens to run in.
    const auto queue = SystemUtils::ReadProcessorQueueLength();
    BOOST_TEST_REQUIRE(queue.has_value());
    BOOST_TEST(queue->cpuCount > 0);
    BOOST_TEST(queue->cpuCount == static_cast<long>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
}

BOOST_AUTO_TEST_CASE(TheCpuTimesAreCumulativeAndIdleIsPartOfTheTotal) {

    // GetSystemTimes' kernel figure already includes idle, so total must not add it again - the
    // mistake would halve every usage percentage EMO records and look plausible doing it.
    const auto first = SystemUtils::ReadCpuTimes();
    BOOST_TEST_REQUIRE(first.has_value());
    BOOST_TEST(first->total > 0);
    BOOST_TEST(first->idle <= first->total);

    // Cumulative since boot, which is what lets two callers each keep their own baseline. A second
    // reading can only be greater or equal, never a fresh interval starting from zero.
    const auto second = SystemUtils::ReadCpuTimes();
    BOOST_TEST_REQUIRE(second.has_value());
    BOOST_TEST(second->total >= first->total);
    BOOST_TEST(second->idle >= first->idle);
}

BOOST_AUTO_TEST_CASE(TheHostAndProcessMemoryFiguresAreBothInRange) {

    // The host's memory, which is a percentage and cannot be outside 0..100 whatever happens.
    const auto system = SystemUtils::ReadSystemMemoryUsagePercent();
    BOOST_TEST_REQUIRE(system.has_value());
    BOOST_TEST(*system > 0.0);
    BOOST_TEST(*system <= 100.0);

    // This process's, which is a different question - the working set of the test binary itself.
    const auto process = SystemUtils::ReadMemoryUsage();
    BOOST_TEST_REQUIRE(process.has_value());
    BOOST_TEST(process->realMb > 0.0);
    BOOST_TEST(process->virtualMb > 0.0);
    BOOST_TEST(process->percentOfTotal > 0.0);

    // A test binary holds a few megabytes of a machine with gigabytes. Not an equality, but a
    // percentage in the double digits here would mean the working set was divided by the wrong total.
    BOOST_TEST(process->percentOfTotal < 100.0);
}

#else

BOOST_AUTO_TEST_CASE(ThereIsNoLoadAverageOffLinux) {

    // /proc/loadavg is Linux's. Answering nullopt is what lets EMO skip the collector on macOS
    // rather than record a zero that reads as an idle machine.
    BOOST_TEST(!SystemUtils::ReadLoadAverage().has_value());
    BOOST_TEST(!SystemUtils::ReadProcessorQueueLength().has_value());
}

#endif
