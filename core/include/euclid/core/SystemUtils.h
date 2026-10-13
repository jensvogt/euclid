// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 12/11/2022.
//

#pragma once

// C++ includes
#include <filesystem>
#include <future>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifdef __APPLE__
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

// Boost includes
#include "boost/asio/deadline_timer.hpp"
#include "boost/asio/ip/host_name.hpp"
#include "boost/asio/read.hpp"
#include "boost/asio/readable_pipe.hpp"
#include "boost/process.hpp"
#include "boost/thread.hpp"

// Euclid includes
#include <euclid/core/LogStream.h>

#define RANDOM_PORT_MIN 32768
#define RANDOM_PORT_MAX 65536
#ifdef _WIN32
#define BUFSIZE 4096
#endif

namespace Euclid::Core {

    /**
     * @brief System utils for command line execution and other system routines.
     *
     * @author jensvogt47\@gmail.com
     */
    class SystemUtils {
    public:
        /**
         * @brief Returns the current working directory.
         *
         * @return absolute path of the current work directory.
         */
        static std::string GetCurrentWorkingDir();

        /**
         * @brief Returns the home directory of the user
         *
         * @return absolute path of the home directory.
         */
        static std::string GetHomeDir();

        /**
         * @brief Returns the root directory for the current machine
         *
         * @par
         * On Linux and macOS this will return "/", on Windows "C:\".
         *
         * @return absolute path of the root directory.
         */
        static std::string GetRootDir();

        /**
         * @brief Returns the DNS host name of the server
         *
         * @return host name of the server
         */
        static std::string GetHostName();

        /**
         * @brief Returns the operating system this binary was built for
         *
         * @par
         * Decided at compile time, so it cannot be configured away: "linux", "windows", "macos",
         * or "unknown" for anything else.
         *
         * @return operating system name
         */
        static std::string GetOperatingSystem();

        /**
         * @brief Returns the CPU architecture this binary was built for
         *
         * @par
         * Decided at compile time, like GetOperatingSystem(), and named as uname -m names it:
         * "x86_64", "aarch64", "arm", "x86", "riscv64", or "unknown" for anything else.
         *
         * @return architecture name
         */
        static std::string GetArchitecture();

        /**
         * @brief Returns the local IP address this host uses to reach another
         *
         * @par
         * Asks the routing table rather than listing interfaces: a UDP socket is "connected" to
         * the remote, which sends nothing, and its local endpoint is read back. On a machine with
         * several interfaces - a LAN, a VPN, a container bridge - that is the one address the
         * remote can be expected to reach this host on, which no interface listing can say.
         *
         * @param host name or address of the remote.
         * @param port port on the remote; only used to pick an endpoint, nothing is sent.
         * @return the local address, or nothing if the remote cannot be resolved or routed to.
         */
        static std::optional<std::string> GetOutboundAddress(const std::string &host, const std::string &port);

        /**
         * @brief Returns every IP address configured on this host's interfaces that are up
         *
         * @par
         * Unlike GetOutboundAddress() this does list interfaces, because the question it answers is
         * the opposite one: not which address this host uses to reach a particular remote, but
         * which addresses a remote might use to reach this host - which is what a server
         * certificate has to name. Loopback and IPv6 link-local addresses are left out: the first
         * are named separately by whoever asks, and the second are not reachable by address alone.
         *
         * @return the addresses as text, IPv4 first; empty if the interfaces cannot be read.
         */
        static std::vector<std::string> GetLocalAddresses();

        /**
         * @brief Returns a random port number between 32768 and 65536
         *
         * @return random port
         */
        //static int GetRandomPort();

        /**
         * @brief Returns the PID of the current process
         *
         * @return PID of the current process
         */
        static int GetPid();

        /**
         * @brief Returns (creating it if necessary) a PID-scoped scratch subdirectory under baseDir.
         *
         * Lets multiple instances of the same module process (e.g. an autoscaled pool spawned by
         * ServiceController) write temp files without colliding, without needing to be told a
         * per-instance path by the manager - each instance derives it the same way autoscaled
         * instance socket paths are derived, from its own pid.
         *
         * @param baseDir base temp directory, e.g. from config key "euclid.temp-dir"
         * @return baseDir + "/" + <this process's pid>, created if it didn't already exist
         */
        static std::string GetInstanceTempDir(const std::string &baseDir);

        /**
         * @brief Quotes one argv element the way CommandLineToArgvW reads it back.
         *
         * @par
         * CreateProcess takes a flat command-line string where execvp() takes an array, so an
         * argument containing a space or a quote has to be escaped such that the child's own argv
         * parser recovers exactly the original bytes. The rule is not obvious - backslashes
         * immediately before a quote, or before the closing quote, are doubled - and getting it
         * wrong does not fail: it silently splits one argument into two, or swallows a quote, and
         * the child receives something almost right.
         *
         * @par
         * In core because two things spawn processes on Windows: euclid-mgr, through
         * Manager::Platform, and euclid-wrk, which runs the applications placed on its host.
         * They do not share a library otherwise, and two copies of this is how two copies come to
         * disagree about a path with a space in it.
         *
         * @par
         * Defined on POSIX too, where nothing calls it: a function that exists only inside an
         * #ifdef cannot be tested on the machine most of this is written on, and the rule it
         * implements is pure string manipulation with no platform in it.
         *
         * @param arg one argv element, unquoted.
         * @return the element, quoted and escaped only if it needs to be.
         */
        [[nodiscard]] static std::string QuoteCommandLineArg(const std::string &arg);

        /**
         * @brief Returns the number of CPU cores
         *
         * @return number of CPU cores
         */
        static int GetNumberOfCores();

        /**
         * @brief One reading of the system's aggregate CPU time counters.
         *
         * idle/total are cumulative since boot, not a point-in-time percentage - callers take the
         * delta between two readings to compute usage over the interval between them. The unit
         * differs by platform (jiffies on Linux, 100-nanosecond intervals on Windows) and
         * deliberately is not normalised: every caller takes a ratio of two deltas, where it
         * cancels.
         */
        struct CpuTimes {
            unsigned long long idle = 0;
            unsigned long long total = 0;
        };

        /**
         * @brief Reads the machine's aggregate CPU time counters: on Linux the "cpu" line of
         * /proc/stat (see man proc(5)), on Windows GetSystemTimes().
         *
         * @par
         * Stateless - callers that want a usage percentage keep their own previous reading and
         * take the delta, since two different call sites polling on different schedules would
         * otherwise corrupt each other's baseline if this kept the "previous" state itself. That is
         * also why the Windows side reads GetSystemTimes() rather than opening a PDH
         * "% Processor Time" query: these are the counters PDH derives that percentage from, and a
         * PDH query reports usage since its own last collect, which is state of exactly the kind
         * two independent pollers cannot share.
         *
         * @return cumulative idle/total CPU time since boot, or std::nullopt if the reading failed
         * or the platform has none (e.g. macOS).
         */
        static std::optional<CpuTimes> ReadCpuTimes();

        /**
         * @brief The percentage of CPU time that was not idle between two readings.
         *
         * @par
         * The ratio the unit cancels out of - see CpuTimes. Here rather than at each call site
         * because there is more than one: the monitoring module reads the manager's host, and a
         * worker reads its own and reports it with its heartbeat. Two copies of three lines of
         * arithmetic is how the two would come to disagree about what "busy" means on a dashboard
         * that puts them side by side.
         *
         * @param previous the earlier reading.
         * @param current the later reading.
         * @return the percentage, or std::nullopt when no time passed between the two readings -
         * which is not 0% busy but no answer, and is what a caller gets on its first poll.
         */
        static std::optional<double> CpuUsagePercent(const CpuTimes &previous, const CpuTimes &current);

        /**
         * @brief The kernel's run-queue averages over the last 1, 5 and 15 minutes, and the number
         * of CPUs they should be read against.
         *
         * @par
         * Load average is not a percentage and does not saturate at 100: it counts the processes
         * that were runnable or in uninterruptible I/O wait, so a figure of 8 means "eight things
         * wanted to run at once". Whether that is idle or desperate depends entirely on how many
         * CPUs there are, which is why the count is reported alongside rather than left to whoever
         * reads the graph - the same 8 is a third of this machine and four times a two-core one.
         */
        struct LoadAverage {
            double oneMinute = 0;
            double fiveMinutes = 0;
            double fifteenMinutes = 0;
            long cpuCount = 0;
        };

        /**
         * @brief Reads /proc/loadavg (Linux-only, see man proc(5)), whose first three fields are
         * the 1-, 5- and 15-minute load averages.
         *
         * @par
         * A direct reading like ReadMemoryUsage() rather than a delta like ReadCpuTimes(): the
         * kernel has already done the averaging, so two calls are not needed and a single one is
         * meaningful on its own.
         *
         * @par
         * Stays Linux-only rather than growing a Windows branch: Windows keeps no run-queue
         * averages at all, and the nearest thing it does keep is an instantaneous count with a
         * reading of its own - see ReadProcessorQueueLength().
         *
         * @return the three averages and the CPU count, or std::nullopt if /proc/loadavg could not
         * be read or parsed (e.g. a non-Linux system).
         */
        static std::optional<LoadAverage> ReadLoadAverage();

        /**
         * @brief How many threads are waiting for a processor, and the number of processors they
         * are waiting for.
         *
         * @par
         * Windows' answer to the load average, and the reason it is a separate reading rather than
         * a LoadAverage with one field filled in: the kernel keeps no 1-, 5- and 15-minute
         * averages, so there is nothing to put in those three fields and computing them here would
         * produce a smoothing that depended on how often the caller happened to poll. This is the
         * instantaneous count, which is what Windows actually maintains.
         *
         * @par
         * Read against the processor count for the same reason a load average is - a queue of 8 is
         * a third of a 24-processor host and four times a two-processor one. The usual guidance is
         * that a sustained queue of more than two per processor is a CPU bottleneck, which makes
         * the per-processor figure the one worth alerting on.
         */
        struct ProcessorQueue {
            double queueLength = 0;
            long cpuCount = 0;
        };

        /**
         * @brief Reads the "\\System\\Processor Queue Length" performance counter (Windows-only).
         *
         * @par
         * Through PDH, which is the only way to reach it - there is no Win32 call for this figure,
         * unlike the CPU times and the memory sizes. Added by its English name
         * (PdhAddEnglishCounter), because counter paths are localised and the literal path above
         * does not resolve on a Windows whose display language is not English.
         *
         * @par
         * A direct reading like ReadLoadAverage() rather than a delta like ReadCpuTimes(): this is
         * an instantaneous count of waiting threads, so one collect is meaningful on its own.
         *
         * @return the queue length and the active processor count, or std::nullopt if the counter
         * could not be read or the platform has none (Linux, macOS).
         */
        static std::optional<ProcessorQueue> ReadProcessorQueueLength();

        /**
         * @brief How much of the machine's memory is in use, as a percentage.
         *
         * @par
         * The machine's, not this process's - ReadMemoryUsage() below answers the latter, which is
         * why every module reports its own small share and no figure for the host existed until
         * this.
         *
         * @par
         * On Linux, read from /proc/meminfo as (MemTotal - MemAvailable) / MemTotal, MemAvailable
         * being what the kernel thinks can be handed out without swapping - a truer "in use" than
         * subtracting MemFree, which counts page cache as used. On Windows the same arithmetic over
         * GlobalMemoryStatusEx's ullTotalPhys and ullAvailPhys, the latter being where the
         * "\\Memory\\Available Bytes" counter gets its value.
         *
         * @return percentage in use, or std::nullopt if the reading failed or the platform has none
         * (e.g. macOS).
         */
        static std::optional<double> ReadSystemMemoryUsagePercent();

        /**
         * @brief This process's real (resident) and virtual memory usage, plus real memory as a
         * percentage of the system's total RAM - the same figures `ps aux`'s RSS/VSZ/%MEM
         * columns report.
         */
        struct MemoryUsage {
            double realMb = 0;
            double virtualMb = 0;
            double percentOfTotal = 0;
        };

        /**
         * @brief Reads this process's current memory usage: on Linux from /proc/self/status (VmRSS,
         * VmSize) and /proc/meminfo (MemTotal), see man proc(5); on Windows from
         * GetProcessMemoryInfo() and GlobalMemoryStatusEx(). Unlike ReadCpuTimes(), this is a direct
         * point-in-time reading - no delta between two calls is needed.
         *
         * @par
         * What "virtual" means differs between the two, unavoidably: Linux's VmSize is the whole
         * address space, where the Windows figure is the commit charge ("\\Process\\Private Bytes").
         * Reporting the literal equivalent there would mean reporting a reservation in the
         * terabytes for every 64-bit process, which is not what anybody watches the series for.
         *
         * @return the reading, or std::nullopt if it failed or the platform has none (e.g. macOS).
         */
        static std::optional<MemoryUsage> ReadMemoryUsage();

        /**
         * @brief Another process's CPU time and memory: what a worker reads for each application
         * instance it started.
         *
         * @par
         * cpuTime is cumulative, like CpuTimes, and in the same unit as CpuTimes::total - clock
         * ticks on Linux, 100-nanosecond intervals on Windows - so ProcessCpuUsagePercent() can
         * divide one delta by the other and the unit cancels. The memory figures are direct
         * readings with the meaning MemoryUsage gives them.
         */
        struct ProcessUsage {
            unsigned long long cpuTime = 0;
            double realMb = 0;
            double virtualMb = 0;
            double percentOfTotal = 0;
        };

        /**
         * @brief Reads the usage of each of the given process groups, summed over every process
         * in the group (Linux-only).
         *
         * @par
         * By group rather than by pid because a worker starts each instance as the leader of a
         * group of its own, and what the instance costs is what the group costs: a launcher script,
         * or a Python application with worker processes, does its work in children the leader's
         * own figures never see. cpuTime includes cutime/cstime, the time of children that exited
         * and were waited for, so a child's CPU time does not vanish from the sum when it exits.
         *
         * @par
         * One pass over /proc for all of them rather than one per group: finding a group's members
         * means reading every process's stat line whichever group is asked for.
         *
         * @param groups the process group ids - each the pid of its leader.
         * @return the usage of each group that has at least one live process; a group with none is
         * absent rather than reported as zero. Empty on any other platform.
         */
        static std::map<int, ProcessUsage> ReadProcessGroupUsage(const std::vector<int> &groups);

        /**
         * @brief Reads the usage of one process through its handle (Windows-only).
         *
         * @par
         * The process alone, not its children. A worker's instances share one job object - the one
         * that takes them down with the worker - so the job's accounting is the whole node's, and
         * there is no per-instance grouping to sum over the way there is on Linux.
         *
         * @param processHandle the HANDLE the process was started with, as void * so this header
         * does not need <windows.h>.
         * @return the reading, or std::nullopt if it failed or this is not Windows.
         */
        static std::optional<ProcessUsage> ReadProcessUsage(void *processHandle);

        /**
         * @brief This process's own cumulative CPU time, user and system, in the unit
         * CpuTimes::total counts in - see ProcessUsage::cpuTime.
         *
         * @par
         * This process alone, unlike ReadProcessGroupUsage(): a module is not necessarily the
         * leader of a group of its own, and summing whatever group it happens to be in would charge
         * it for its siblings. On Linux from /proc/self/stat, on Windows from GetProcessTimes().
         *
         * @return the reading, or std::nullopt if it failed or the platform has none (e.g. macOS).
         */
        static std::optional<unsigned long long> ReadProcessCpuTime();

        /**
         * @brief The share of the whole machine's CPU time a process used between two readings.
         *
         * @par
         * Of the machine, not of one core: 100 is every processor busy with this process alone,
         * so the figure sits on the same scale as the host's "system-cpu-usage" and the two can be
         * read against each other. A process keeping one core of sixteen busy reads 6.25.
         *
         * @return the percentage, or std::nullopt when no time passed or either counter went
         * backwards - a process in the group that left it, or a reading across a pid being reused.
         */
        static std::optional<double> ProcessCpuUsagePercent(unsigned long long previousProcessTime, unsigned long long currentProcessTime,
                                                             const CpuTimes &previousSystem, const CpuTimes &currentSystem);

        /**
         * @brief Returns the value of an environment variable or empty string, if not existent.
         *
         * @param name of the environment variable
         * @return value of the environment variable as string
         */
        static std::string GetEnvironmentVariableValue(const std::string &name);

        /**
         * @brief Returns true if environment variable exists.
         *
         * @param name of the environment variable
         * @return true if existent, otherwise false
         */
        static bool HasEnvironmentVariable(const std::string &name);

        /**
         * @brief Return the next free port number
         *
         * @return next free port number
         */
        static int GetNextFreePort();
    };

} // namespace Euclid::Core