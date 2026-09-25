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