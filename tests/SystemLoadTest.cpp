// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE SystemLoadTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <algorithm>
#include <fstream>
#include <sstream>

#include <boost/asio/ip/address.hpp>
#include <string>

// Euclid includes
#include <euclid/core/SystemUtils.h>

#ifdef _WIN32
#include <windows.h>    // GetActiveProcessorCount, for checking the count reported beside the queue
#endif
#ifdef __linux__
#include <chrono>
#include <csignal>      // kill
#include <thread>
#include <sys/wait.h>   // waitpid
#include <unistd.h>     // fork, setpgid
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

// A worker reads each application instance as a process group: the instance is the leader of a
// group of its own, and what it costs includes whatever it started. These start real groups,
// because the parsing of /proc/<pid>/stat is what can be wrong - one field off and the figures are
// another column of the same line, plausible and meaningless.
namespace {

    // Spins until killed, so it has CPU time to find.
    [[noreturn]] void burn() {
        volatile unsigned long long counter = 0;
        while (true) counter = counter + 1;
    }

    // Starts a group whose leader sleeps and, if @p busyChild, whose one child burns CPU - so any
    // CPU time the group shows can only have come from the child. Returns the leader's pid.
    pid_t startGroup(const bool busyChild) {
        const auto leader = fork();
        if (leader == 0) {
            setpgid(0, 0);
            if (busyChild && fork() == 0) burn();
            while (true) pause();
        }
        setpgid(leader, leader);// in the parent too, so the group exists before the first read
        return leader;
    }

    void stopGroup(const pid_t leader) {
        kill(-leader, SIGKILL);
        waitpid(leader, nullptr, 0);
    }
}// namespace

BOOST_AUTO_TEST_CASE(AGroupsUsageIsReadFromItsProcesses) {

    const auto leader = startGroup(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const auto usage = SystemUtils::ReadProcessGroupUsage({leader});
    stopGroup(leader);

    BOOST_TEST_REQUIRE(usage.contains(leader));
    const auto &group = usage.at(leader);

    // A forked copy of this test binary: resident memory of at least a page, an address space far
    // larger than that, and a sliver of the machine. Not compared exactly - what is pinned is that
    // the sizes were taken from their own fields and converted with the right units.
    //
    // Resident memory above zero is also the regression check for where it is read from: stat's
    // own rss field reported 0 for exactly this child, a moment after the fork, on a 7.0 kernel.
    BOOST_TEST(group.realMb > 0.0);
    BOOST_TEST(group.virtualMb > group.realMb);
    BOOST_TEST(group.percentOfTotal > 0.0);
    BOOST_TEST(group.percentOfTotal < 50.0);
}

BOOST_AUTO_TEST_CASE(TheCpuOfAChildIsTheGroups) {

    // The leader only sleeps, so the time can only be the child's - which is the case reading the
    // leader's pid alone gets wrong for a launcher script or a Python application with workers.
    const auto leader = startGroup(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto systemBefore = SystemUtils::ReadCpuTimes();
    const auto before = SystemUtils::ReadProcessGroupUsage({leader});

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const auto systemAfter = SystemUtils::ReadCpuTimes();
    const auto after = SystemUtils::ReadProcessGroupUsage({leader});
    stopGroup(leader);

    BOOST_TEST_REQUIRE(before.contains(leader));
    BOOST_TEST_REQUIRE(after.contains(leader));
    BOOST_TEST_REQUIRE(systemBefore.has_value());
    BOOST_TEST_REQUIRE(systemAfter.has_value());
    BOOST_TEST(after.at(leader).cpuTime > before.at(leader).cpuTime);

    // A child busy for the whole interval keeps about one processor busy, which as a share of the
    // machine is roughly 100 / processors - loosely, since the host is doing other things too.
    const auto percent = SystemUtils::ProcessCpuUsagePercent(before.at(leader).cpuTime, after.at(leader).cpuTime,
                                                             *systemBefore, *systemAfter);
    BOOST_TEST_REQUIRE(percent.has_value());
    const auto processors = static_cast<double>(sysconf(_SC_NPROCESSORS_ONLN));
    BOOST_TEST(*percent > 25.0 / processors);
    BOOST_TEST(*percent <= 100.0);
}

BOOST_AUTO_TEST_CASE(AGroupWithNoProcessesIsAbsent) {

    // Started and stopped, so the pid names nothing - absent rather than zero, which would read as
    // an idle instance rather than a missing one.
    const auto leader = startGroup(false);
    stopGroup(leader);

    BOOST_TEST(!SystemUtils::ReadProcessGroupUsage({leader}).contains(leader));
    BOOST_TEST(SystemUtils::ReadProcessGroupUsage({}).empty());
}

BOOST_AUTO_TEST_CASE(AModulesCpuIsItsOwnNotTheHosts) {

    // What "euclid-cpu-usage" is computed from. It used to be the host's CPU under every module's
    // label; this pins that it is this process's own time - it grows while this thread spins, and
    // a child spinning in another process does not move it.
    const auto before = SystemUtils::ReadProcessCpuTime();
    BOOST_TEST_REQUIRE(before.has_value());

    const auto busyUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    volatile unsigned long long counter = 0;
    while (std::chrono::steady_clock::now() < busyUntil) counter = counter + 1;

    const auto afterSpinning = SystemUtils::ReadProcessCpuTime();
    BOOST_TEST_REQUIRE(afterSpinning.has_value());
    BOOST_TEST(*afterSpinning > *before);

    // A busy group of its own beside this one, while this thread sleeps: a reading of the host
    // would climb by about a core's worth, this one by next to nothing.
    const auto leader = startGroup(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const auto afterSleeping = SystemUtils::ReadProcessCpuTime();
    const auto group = SystemUtils::ReadProcessGroupUsage({leader});
    stopGroup(leader);

    BOOST_TEST_REQUIRE(afterSleeping.has_value());
    BOOST_TEST_REQUIRE(group.contains(leader));
    BOOST_TEST(*afterSleeping - *afterSpinning < group.at(leader).cpuTime);
}

BOOST_AUTO_TEST_CASE(OnlyTheGroupsAskedForAreRead) {

    const auto first = startGroup(false);
    const auto second = startGroup(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const auto usage = SystemUtils::ReadProcessGroupUsage({first});
    stopGroup(first);
    stopGroup(second);

    BOOST_TEST(usage.size() == 1u);
    BOOST_TEST(usage.contains(first));
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

BOOST_AUTO_TEST_CASE(CpuUsageIsTheNonIdleShareOfTheInterval) {

    // Fixed readings rather than two real ones, because what is being pinned is the arithmetic and
    // a live host gives a different answer every run. One implementation of it, because EMO reads
    // the manager's host and a worker reads its own: two copies is how the two hosts come to
    // disagree about what "busy" means on a dashboard that shows them side by side.
    BOOST_TEST(SystemUtils::CpuUsagePercent({.idle = 0, .total = 0}, {.idle = 250, .total = 1000}).value_or(-1) == 75.0);
    BOOST_TEST(SystemUtils::CpuUsagePercent({.idle = 100, .total = 200}, {.idle = 100, .total = 300}).value_or(-1) == 100.0);
    BOOST_TEST(SystemUtils::CpuUsagePercent({.idle = 100, .total = 200}, {.idle = 200, .total = 300}).value_or(-1) == 0.0);

    // No time passed, so there is no answer - which is not the same as 0% busy, and is what a
    // caller gets on its first poll before it has a baseline.
    BOOST_TEST(!SystemUtils::CpuUsagePercent({.idle = 100, .total = 200}, {.idle = 100, .total = 200}).has_value());

    // Counters that went backwards: a reading across a suspend, or one taken against a different
    // baseline. Unsigned subtraction would turn that into an enormous positive percentage.
    BOOST_TEST(!SystemUtils::CpuUsagePercent({.idle = 100, .total = 400}, {.idle = 100, .total = 300}).has_value());
    BOOST_TEST(!SystemUtils::CpuUsagePercent({.idle = 200, .total = 200}, {.idle = 100, .total = 300}).has_value());

    // Idle growing faster than total cannot happen on one host, and would report a negative
    // percentage if it were believed.
    BOOST_TEST(!SystemUtils::CpuUsagePercent({.idle = 0, .total = 0}, {.idle = 200, .total = 100}).has_value());
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

BOOST_AUTO_TEST_CASE(AProcessesCpuIsItsShareOfTheWholeMachine) {

    // Fixed readings, as for CpuUsagePercent: the arithmetic is what is pinned. A process that used
    // 100 ticks of the 800 the machine counted is an eighth of it - one core of eight, fully busy.
    const SystemUtils::CpuTimes before{.idle = 0, .total = 1000};
    const SystemUtils::CpuTimes after{.idle = 0, .total = 1800};
    BOOST_TEST(SystemUtils::ProcessCpuUsagePercent(50, 150, before, after).value_or(-1) == 12.5);
    BOOST_TEST(SystemUtils::ProcessCpuUsagePercent(50, 50, before, after).value_or(-1) == 0.0);

    // Two readings taken a moment apart can make a process busy on every core come out a hair over
    // the machine's own count; more than all of the machine is not a reading.
    BOOST_TEST(SystemUtils::ProcessCpuUsagePercent(0, 900, before, after).value_or(-1) == 100.0);

    // No time passed, or a counter went backwards - a process that left the group, or a pid reused
    // between two readings. Refused, rather than an enormous unsigned difference.
    BOOST_TEST(!SystemUtils::ProcessCpuUsagePercent(50, 150, before, before).has_value());
    BOOST_TEST(!SystemUtils::ProcessCpuUsagePercent(150, 50, before, after).has_value());
    BOOST_TEST(!SystemUtils::ProcessCpuUsagePercent(50, 150, after, before).has_value());
}

BOOST_AUTO_TEST_CASE(LocalAddressesAreReachableOnes) {

    // What a generated gateway certificate names, so every entry has to be an address a client
    // could dial: parseable, not loopback (named separately), not link-local, and each only once.
    // Not required to be non-empty - a build machine with no network is still a build machine.
    const auto addresses = SystemUtils::GetLocalAddresses();
    for (const auto &text: addresses) {
        boost::system::error_code ec;
        const auto address = boost::asio::ip::make_address(text, ec);
        BOOST_TEST_REQUIRE(!ec, text);
        BOOST_TEST(!address.is_loopback(), text);
        BOOST_TEST(!(address.is_v6() && address.to_v6().is_link_local()), text);
        BOOST_TEST(std::count(addresses.begin(), addresses.end(), text) == 1, text);
    }
}
