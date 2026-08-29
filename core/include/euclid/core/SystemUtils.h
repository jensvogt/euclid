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
     * @author jens.vogt\@opitz-consulting.com
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
         * @brief One reading of the Linux system's aggregate CPU time counters, from /proc/stat.
         *
         * idle/total are cumulative jiffies since boot, not a point-in-time percentage - callers
         * take the delta between two readings to compute usage over the interval between them.
         */
        struct CpuTimes {
            unsigned long long idle = 0;
            unsigned long long total = 0;
        };

        /**
         * @brief Reads the aggregate "cpu" line of /proc/stat (Linux-only, see man proc(5)):
         * "cpu  user nice system idle iowait irq softirq steal guest guest_nice".
         *
         * Stateless - callers that want a usage percentage keep their own previous reading and
         * take the delta, since two different call sites polling on different schedules would
         * otherwise corrupt each other's baseline if this kept the "previous" state itself.
         *
         * @return idle/total jiffies since boot, or std::nullopt if /proc/stat couldn't be read
         * or parsed (e.g. non-Linux system).
         */
        static std::optional<CpuTimes> ReadCpuTimes();

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
         * @brief Reads this process's current memory usage from /proc/self/status (VmRSS,
         * VmSize) and /proc/meminfo (MemTotal), Linux-only (see man proc(5)). Unlike
         * ReadCpuTimes(), this is a direct point-in-time reading - no delta between two calls is
         * needed.
         *
         * @return the reading, or std::nullopt if /proc/self/status or /proc/meminfo couldn't be
         * read or parsed (e.g. non-Linux system).
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
         * @brief Run command in a shell
         *
         * @param shellcmd command
         * @param args vector of string arguments
         * @param output output stream
         * @param error error stream
         */
        static void RunShellCommand(const std::string &shellcmd, const std::vector<std::string> &args, std::string &output, std::string &error);

        /**
         * @brief Return the next free port number
         *
         * @return next free port number
         */
        static int GetNextFreePort();
    };

} // namespace Euclid::Core