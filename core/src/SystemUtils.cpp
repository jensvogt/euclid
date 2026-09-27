// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 12/11/2022.
//

#include <../include/euclid/core/SystemUtils.h>
#ifdef _WIN32
#include <windows.h>    // GetEnvironmentVariable, GetSystemTimes, GlobalMemoryStatusEx
#include <pdh.h>        // PdhOpenQuery/PdhAddEnglishCounter - the performance counter API
#include <pdhmsg.h>     // PDH_CSTATUS_VALID_DATA
#include <psapi.h>      // GetProcessMemoryInfo
#else
#include <unistd.h>     // getuid
#include <pwd.h>        // getpwuid_r
#include <sys/types.h>  // uid_t
#endif
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace Euclid::Core {

    std::string SystemUtils::GetCurrentWorkingDir() {
        return boost::filesystem::current_path().string();
    }

    std::string SystemUtils::GetHomeDir() {

#ifdef _WIN32
        // Use WinAPI — handles unicode, no manual buffer management
        char buffer[MAX_PATH];
        DWORD len = GetEnvironmentVariableA("USERPROFILE", buffer, MAX_PATH);
        if (len == 0 || len > MAX_PATH) {
            // Fallback: HOMEDRIVE + HOMEPATH
            char drive[64], path[MAX_PATH];
            GetEnvironmentVariableA("HOMEDRIVE", drive, sizeof(drive));
            GetEnvironmentVariableA("HOMEPATH", path, sizeof(path));
            return std::string(drive) + std::string(path);
        }
        return std::string(buffer, len);

#else

        // Try $HOME environment variable first (single call, no race)
        if (const char *home = getenv("HOME"); home && *home != '\0') return {home};

        // 2. Fall back to passwd entry — use thread-safe getpwuid_r
        const uid_t uid = getuid();
        const long bufSize = sysconf(_SC_GETPW_R_SIZE_MAX);
        const std::size_t size = (bufSize > 0) ? static_cast<std::size_t>(bufSize) : 4096;

        std::string buf(size, '\0');
        passwd pwd{};
        passwd *result = nullptr;

        if (const int rc = getpwuid_r(uid, &pwd, buf.data(), buf.size(), &result); rc != 0) {
            throw std::runtime_error("getpwuid_r failed: " + std::string(strerror(rc)));
        }
        if (result == nullptr) throw std::runtime_error("No passwd entry for uid " + std::to_string(uid));

        return {result->pw_dir};
#endif
    }

    std::string SystemUtils::GetRootDir() {
        const boost::filesystem::path path;
        return path.root_directory().string();
    }

    std::string SystemUtils::GetHostName() {
        return boost::asio::ip::host_name();
    }

    //
    // int SystemUtils::GetRandomPort() {
    //     return RandomUtils::NextInt(RANDOM_PORT_MIN, RANDOM_PORT_MAX);
    // }

    int SystemUtils::GetPid() {
#ifdef WIN32
        return _getpid();
#else
        return getpid();
#endif
    }

    std::string SystemUtils::GetInstanceTempDir(const std::string &baseDir) {
        const std::string path = baseDir + "/" + std::to_string(GetPid());
        boost::system::error_code ec;
        boost::filesystem::create_directories(path, ec);
        if (ec) log_warning << "Could not create instance temp dir, path: " << path << ", error: " << ec.message();
        return path;
    }

    int SystemUtils::GetNextFreePort() {
        sockaddr_in sin{};
        constexpr int port = 0;

        const int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == -1) return -1;

        sin.sin_port = htons(port);
        sin.sin_addr.s_addr = 0;
        sin.sin_addr.s_addr = INADDR_ANY;
        sin.sin_family = AF_INET;

        if (bind(s, reinterpret_cast<sockaddr *>(&sin), sizeof(sockaddr_in)) == -1) {
            if (errno == EADDRINUSE)
                log_error << "Port in use";
            return -1;
        }
        socklen_t len = sizeof(sin);
        if (getsockname(s, reinterpret_cast<sockaddr *>(&sin), &len) != -1) {
#ifdef _WIN32
            closesocket(s);
#else
            shutdown(s, SHUT_RDWR);
            close(s);
#endif
            return ntohs(sin.sin_port);
        }
        return -1;
    }

    int SystemUtils::GetNumberOfCores() {
        return static_cast<int>(boost::thread::hardware_concurrency());
    }

    std::optional<double> SystemUtils::ReadSystemMemoryUsagePercent() {
#ifdef __linux__
        std::ifstream meminfo("/proc/meminfo");
        if (!meminfo.is_open()) return std::nullopt;

        unsigned long long totalKb = 0;
        unsigned long long availableKb = 0;
        std::string line;
        while (std::getline(meminfo, line)) {
            std::istringstream iss(line);
            std::string label;
            unsigned long long value = 0;
            iss >> label >> value;
            if (!iss) continue;
            if (label == "MemTotal:") totalKb = value;
            else if (label == "MemAvailable:") availableKb = value;
            // Both are near the top of the file; nothing after them is needed.
            if (totalKb > 0 && availableKb > 0) break;
        }

        // MemAvailable has been in /proc/meminfo since Linux 3.14. Without it there is no honest
        // answer here - MemFree counts the page cache as used, which reads as a machine at 90%
        // when it is idle - so this reports nothing rather than something misleading.
        if (totalKb == 0 || availableKb == 0) return std::nullopt;

        const auto used = totalKb > availableKb ? totalKb - availableKb : 0;
        return 100.0 * static_cast<double>(used) / static_cast<double>(totalKb);
#elif defined(_WIN32)
        // The same two figures the Memory counter set reports - "\Memory\Available Bytes" against
        // the machine's installed physical memory - read through GlobalMemoryStatusEx rather than
        // PDH because that is where PDH itself gets them, and this way there is no query to open.
        //
        // ullAvailPhys is Windows' MemAvailable: what can be handed out without paging, standby
        // pages included. Subtracting it is the same arithmetic as the Linux branch above, and it
        // is right for the same reason - the cache is not "used".
        MEMORYSTATUSEX status{};
        status.dwLength = sizeof(status);
        if (!GlobalMemoryStatusEx(&status) || status.ullTotalPhys == 0) return std::nullopt;

        const auto used = status.ullTotalPhys > status.ullAvailPhys ? status.ullTotalPhys - status.ullAvailPhys : 0;
        return 100.0 * static_cast<double>(used) / static_cast<double>(status.ullTotalPhys);
#else
        return std::nullopt;
#endif
    }

    std::optional<SystemUtils::CpuTimes> SystemUtils::ReadCpuTimes() {
#ifdef __linux__
        std::ifstream stat("/proc/stat");
        if (!stat.is_open()) return std::nullopt;

        std::string line;
        if (!std::getline(stat, line)) return std::nullopt;

        std::istringstream iss(line);
        std::string label;
        unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
        iss >> label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
        if (!iss || label != "cpu") return std::nullopt;

        CpuTimes times;
        times.idle = idle + iowait;
        times.total = user + nice + system + idle + iowait + irq + softirq + steal;
        return times;
#elif defined(_WIN32)
        // GetSystemTimes rather than a PDH "% Processor Time" query, although both answer the same
        // question: these are the cumulative counters that PDH computes that percentage from, and
        // cumulative is what this function has to return. A PDH query reports usage since its own
        // previous collect, and there are two independent callers here polling on their own
        // schedules (EmoServer and HttpActionServer, each keeping its own previous reading) - one
        // shared query would hand each of them the other's interval. See the header.
        FILETIME idleTime{}, kernelTime{}, userTime{};
        if (!GetSystemTimes(&idleTime, &kernelTime, &userTime)) return std::nullopt;

        // 100-nanosecond intervals since boot, split across two 32-bit halves. Not jiffies, but
        // nothing downstream cares: every caller takes a ratio of two deltas.
        const auto ticks = [](const FILETIME &fileTime) {
            return static_cast<unsigned long long>(fileTime.dwHighDateTime) << 32 | fileTime.dwLowDateTime;
        };

        CpuTimes times;
        times.idle = ticks(idleTime);
        // Kernel time already includes idle time on Windows, so this is the whole of it - adding
        // idle again would inflate the denominator and read as a machine half as busy as it is.
        times.total = ticks(kernelTime) + ticks(userTime);
        return times;
#else
        return std::nullopt;
#endif
    }

    std::optional<SystemUtils::LoadAverage> SystemUtils::ReadLoadAverage() {
#ifdef __linux__
        std::ifstream loadavg("/proc/loadavg");
        if (!loadavg.is_open()) return std::nullopt;

        // "0.45 0.62 0.71 2/1483 29174" - the three averages, then running/total tasks and the
        // last pid. Only the first three are read; the rest are a different question.
        LoadAverage load;
        loadavg >> load.oneMinute >> load.fiveMinutes >> load.fifteenMinutes;
        if (!loadavg) return std::nullopt;

        // Reported with the averages rather than left to the caller, because a load average means
        // nothing without it - see LoadAverage. Zero rather than one when the count is unknown, so
        // a caller dividing by it has something obviously wrong to notice rather than a figure
        // that looks plausible and is out by the width of the machine.
        const auto processors = sysconf(_SC_NPROCESSORS_ONLN);
        load.cpuCount = processors > 0 ? static_cast<long>(processors) : 0;

        return load;
#else
        return std::nullopt;
#endif
    }

    std::optional<SystemUtils::ProcessorQueue> SystemUtils::ReadProcessorQueueLength() {
#ifdef _WIN32
        // The one figure here that genuinely needs PDH: the kernel exposes no call for it, and
        // "\System\Processor Queue Length" is the counter Windows keeps instead of a load average.
        PDH_HQUERY query = nullptr;
        if (PdhOpenQueryA(nullptr, 0, &query) != ERROR_SUCCESS) return std::nullopt;

        // Every path out of here closes the query, including the failures below. A leaked PDH query
        // is a leaked handle on a collector that runs once a minute forever.
        struct QueryGuard {
            PDH_HQUERY handle;
            ~QueryGuard() {
                if (handle) PdhCloseQuery(handle);
            }
        } guard{query};

        // The English name, via PdhAddEnglishCounter rather than PdhAddCounter: counter paths are
        // localised, so "\System\Processor Queue Length" does not exist on a German or Japanese
        // Windows and PdhAddCounter would fail there with PDH_CSTATUS_NO_COUNTER. The English
        // variant looks up the counter by index and works whatever the display language is.
        PDH_HCOUNTER counter = nullptr;
        if (PdhAddEnglishCounterA(query, "\\System\\Processor Queue Length", 0, &counter) != ERROR_SUCCESS) return std::nullopt;

        // One collect is enough, unlike a rate counter: this is an instantaneous count of threads
        // waiting, so there is no interval to divide by and nothing to prime.
        if (PdhCollectQueryData(query) != ERROR_SUCCESS) return std::nullopt;

        PDH_FMT_COUNTERVALUE value{};
        if (PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, nullptr, &value) != ERROR_SUCCESS) return std::nullopt;
        if (value.CStatus != PDH_CSTATUS_VALID_DATA && value.CStatus != PDH_CSTATUS_NEW_DATA) return std::nullopt;

        ProcessorQueue queue;
        queue.queueLength = value.doubleValue;

        // GetActiveProcessorCount over all groups rather than GetNumberOfCores(): a host with more
        // than 64 processors is split into groups, and the per-group figure would make a busy
        // machine read as a desperate one. Zero when unknown, same as ReadLoadAverage().
        const auto processors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        queue.cpuCount = processors > 0 ? static_cast<long>(processors) : 0;

        return queue;
#else
        return std::nullopt;
#endif
    }

    std::optional<SystemUtils::MemoryUsage> SystemUtils::ReadMemoryUsage() {
#ifdef __linux__
        std::ifstream status("/proc/self/status");
        if (!status.is_open()) return std::nullopt;

        unsigned long long vmRssKb = 0, vmSizeKb = 0;
        bool haveRss = false, haveSize = false;
        std::string line;
        while ((!haveRss || !haveSize) && std::getline(status, line)) {
            std::istringstream iss(line);
            std::string label;
            unsigned long long value;
            iss >> label >> value;
            if (label == "VmRSS:") {
                vmRssKb = value;
                haveRss = true;
            } else if (label == "VmSize:") {
                vmSizeKb = value;
                haveSize = true;
            }
        }
        if (!haveRss || !haveSize) return std::nullopt;

        std::ifstream meminfo("/proc/meminfo");
        if (!meminfo.is_open()) return std::nullopt;

        unsigned long long memTotalKb = 0;
        while (std::getline(meminfo, line)) {
            std::istringstream iss(line);
            std::string label;
            unsigned long long value;
            iss >> label >> value;
            if (label == "MemTotal:") {
                memTotalKb = value;
                break;
            }
        }
        if (memTotalKb == 0) return std::nullopt;

        MemoryUsage usage;
        usage.realMb = static_cast<double>(vmRssKb) / 1024.0;
        usage.virtualMb = static_cast<double>(vmSizeKb) / 1024.0;
        usage.percentOfTotal = 100.0 * static_cast<double>(vmRssKb) / static_cast<double>(memTotalKb);
        return usage;
#elif defined(_WIN32)
        // The Process counter set's two sizes for this process, from GetProcessMemoryInfo rather
        // than a per-process PDH query - a PDH instance path is "\Process(euclid-mon#2)\...", where
        // the "#2" is assigned by enumeration order and moves when another instance of the same
        // module exits. A pool that scales would silently start reporting a sibling's memory.
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters), sizeof(counters))) {
            return std::nullopt;
        }

        MEMORYSTATUSEX status{};
        status.dwLength = sizeof(status);
        if (!GlobalMemoryStatusEx(&status) || status.ullTotalPhys == 0) return std::nullopt;

        MemoryUsage usage;
        // WorkingSetSize is "\Process\Working Set", the resident pages - VmRSS, and the same figure
        // the task manager's "memory" column shows.
        usage.realMb = static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
        // PrivateUsage is "\Process\Private Bytes", the commit charge. Reported as the virtual
        // figure deliberately: Windows' literal VmSize equivalent counts every reservation the
        // address space holds, which on a 64-bit process is a number in the terabytes and says
        // nothing about the process. Committed memory is what VmSize is watched for.
        usage.virtualMb = static_cast<double>(counters.PrivateUsage) / (1024.0 * 1024.0);
        usage.percentOfTotal = 100.0 * static_cast<double>(counters.WorkingSetSize) / static_cast<double>(status.ullTotalPhys);
        return usage;
#else
        return std::nullopt;
#endif
    }

    std::string SystemUtils::GetEnvironmentVariableValue(const std::string &name) {
#ifdef _WIN32
        auto pValue = static_cast<LPTSTR>(malloc(BUFSIZE * sizeof(TCHAR)));
        memset(pValue, 0, BUFSIZE);
        if (const DWORD result = GetEnvironmentVariable(name.c_str(), pValue, BUFSIZE); !result) {
            log_trace << "Environment variable not found, name: " << name << ", error: " << result;
            return {};
        }
        return {pValue};
#else
        if (getenv(name.c_str()) == nullptr) {
            log_debug << "Environment variable not found, name: " << name;
            return {};
        }
        return {getenv(name.c_str())};
#endif
    }

    bool SystemUtils::HasEnvironmentVariable(const std::string &name) {
        return !GetEnvironmentVariableValue(name).empty();
    }

} // namespace Euclid::Core