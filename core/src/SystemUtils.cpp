//
// Created by vogje01 on 12/11/2022.
//

#include <../include/euclid/core/SystemUtils.h>
#ifdef _WIN32
#include <windows.h>    // GetEnvironmentVariable
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

    void SystemUtils::RunShellCommand(const std::string &shellcmd, const std::vector<std::string> &args, std::string &output, std::string &error) {
        // TODO: fix me
        //log_debug << "Running shell command, cmd: " << shellcmd << ", args: " << StringUtils::Join(args);

        boost::filesystem::path awsExe = boost::process::v2::environment::find_executable("aws");
        // TODO: fix me
        // log_debug << "Running shell command, cmd: " << awsExe << ", args: " << StringUtils::Join(args);

        boost::asio::io_context ctx;
        boost::asio::readable_pipe outPipe{ctx};
        boost::asio::readable_pipe errPipe{ctx};

#ifdef _WIN32
        boost::process::process proc(ctx, awsExe.string(), args, boost::process::process_stdio{{}, outPipe, errPipe});
#else
        boost::process::process proc(ctx, awsExe, args, boost::process::process_stdio{{}, outPipe, errPipe});
#endif

        boost::system::error_code outEc, errEc;
        boost::asio::async_read(outPipe, boost::asio::dynamic_buffer(output), [&](const boost::system::error_code &ec, std::size_t) { outEc = ec; });
        boost::asio::async_read(errPipe, boost::asio::dynamic_buffer(error), [&](const boost::system::error_code &ec, std::size_t) { errEc = ec; });

        ctx.run();
        proc.wait();

        // Helper: both EOF and broken_pipe are normal pipe-closed signals
        auto isPipeClose = [](const boost::system::error_code &ec) {
            return !ec || ec == boost::asio::error::eof || ec == boost::asio::error::broken_pipe
#ifdef _WIN32
                   || ec == boost::system::error_code(109, boost::system::system_category()) // ERROR_BROKEN_PIPE
#endif
                    ;
        };

        if (!isPipeClose(outEc))
            log_error << "stdout read error: " << outEc.message() << " (" << outEc.value() << ")";
        if (!isPipeClose(errEc))
            log_error << "stderr read error: " << errEc.message() << " (" << errEc.value() << ")";
    }

} // namespace Euclid::Core