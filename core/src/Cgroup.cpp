// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <tuple>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// Euclid includes
#include <euclid/core/Cgroup.h>

namespace Euclid::Core::Cgroup {

    namespace {

        // The controllers an instance group is given, in the order they are asked for. memory is
        // the one this exists for and the tree is not enabled without it; cpu and pids are taken
        // when the host offers them.
        [[maybe_unused]] constexpr std::string_view kRequired = "memory";
        [[maybe_unused]] constexpr std::string_view kOptional[] = {"cpu", "pids"};

        std::optional<std::string> readText(const std::filesystem::path &path) {
            std::ifstream in(path);
            if (!in) return std::nullopt;
            std::ostringstream text;
            text << in.rdbuf();
            return text.str();
        }

        std::string_view trim(std::string_view text) {
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\n' || text.front() == '\r')) text.remove_prefix(1);
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' || text.back() == '\r')) text.remove_suffix(1);
            return text;
        }

        std::optional<long long> toNumber(std::string_view text) {
            text = trim(text);
            if (text.empty()) return std::nullopt;
            long long value = 0;
            for (const char c: text) {
                if (c < '0' || c > '9') return std::nullopt;
                value = value * 10 + (c - '0');
            }
            return value;
        }

#ifndef _WIN32
        // One write(2), which is what every cgroup interface file wants: a write is one command,
        // and a stream that split "+memory +cpu" over two would be two half-commands. The error,
        // when there is one, in words, for the reason Setup() gives.
        std::optional<std::string> writeFile(const std::filesystem::path &path, const std::string &text) {
            const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (fd < 0) return path.string() + ": " + std::strerror(errno);
            const auto written = ::write(fd, text.data(), text.size());
            const int error = errno;
            ::close(fd);
            if (written != static_cast<ssize_t>(text.size())) return path.string() + ": " + std::strerror(error);
            return std::nullopt;
        }

        std::optional<std::string> makeDirectory(const std::filesystem::path &path) {
            if (::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST) return std::nullopt;
            return path.string() + ": " + std::strerror(errno);
        }
#endif

    }// namespace

    std::optional<std::string> ParseSelfCgroup(const std::string_view procSelfCgroup) {
        // v2 is the line whose hierarchy id is 0 and whose controller list is empty. A hybrid host
        // has v1 lines as well; they are not ours to use.
        std::size_t start = 0;
        while (start < procSelfCgroup.size()) {
            auto end = procSelfCgroup.find('\n', start);
            if (end == std::string_view::npos) end = procSelfCgroup.size();
            if (const auto line = trim(procSelfCgroup.substr(start, end - start)); line.starts_with("0::")) {
                auto path = line.substr(3);
                if (path.empty() || path.front() != '/') return std::nullopt;
                path.remove_prefix(1);
                return std::string(path);
            }
            start = end + 1;
        }
        return std::nullopt;
    }

    std::optional<long long> KeyedValue(const std::string_view text, const std::string_view key) {
        std::size_t start = 0;
        while (start < text.size()) {
            auto end = text.find('\n', start);
            if (end == std::string_view::npos) end = text.size();
            const auto line = text.substr(start, end - start);
            if (const auto space = line.find(' '); space != std::string_view::npos && line.substr(0, space) == key) {
                return toNumber(line.substr(space + 1));
            }
            start = end + 1;
        }
        return std::nullopt;
    }

    std::optional<long long> SingleValue(const std::string_view text) {
        if (trim(text) == "max") return -1;
        return toNumber(text);
    }

    std::optional<double> ParseCpuMax(const std::string_view text) {
        const auto trimmed = trim(text);
        const auto space = trimmed.find(' ');
        if (space == std::string_view::npos) return std::nullopt;
        const auto period = toNumber(trimmed.substr(space + 1));
        if (!period.has_value() || *period <= 0) return std::nullopt;
        if (const auto quota = trimmed.substr(0, space); quota == "max") return -1.0;
        const auto quota = toNumber(trimmed.substr(0, space));
        if (!quota.has_value()) return std::nullopt;
        return static_cast<double>(*quota) / static_cast<double>(*period);
    }

    std::optional<double> ThrottledPercent(const long long previousPeriods, const long long previousThrottled,
                                           const long long periods, const long long throttled) {
        if (previousPeriods < 0 || previousThrottled < 0 || periods < 0 || throttled < 0) return std::nullopt;
        if (periods <= previousPeriods || throttled < previousThrottled) return std::nullopt;
        return 100.0 * static_cast<double>(throttled - previousThrottled) / static_cast<double>(periods - previousPeriods);
    }

    bool HasController(const std::string_view controllers, const std::string_view controller) {
        std::size_t start = 0;
        while (start < controllers.size()) {
            auto end = controllers.find_first_of(" \n", start);
            if (end == std::string_view::npos) end = controllers.size();
            if (controllers.substr(start, end - start) == controller) return true;
            start = end + 1;
        }
        return false;
    }

    std::optional<Usage> ReadUsage(const std::filesystem::path &group) {

        std::error_code ec;
        if (!std::filesystem::is_directory(group, ec)) return std::nullopt;

        Usage usage;
        const auto single = [&group](const char *file) -> long long {
            const auto text = readText(group / file);
            if (!text.has_value()) return -1;
            return SingleValue(*text).value_or(-1);
        };
        const auto keyed = [&group](const char *file, const char *key) -> std::optional<long long> {
            const auto text = readText(group / file);
            if (!text.has_value()) return std::nullopt;
            return KeyedValue(*text, key);
        };

        usage.memoryBytes = single("memory.current");
        usage.memoryPeakBytes = single("memory.peak");
        usage.memoryMaxBytes = single("memory.max");
        usage.oomKills = keyed("memory.events", "oom_kill").value_or(-1);
        usage.cpuUsageUsec = keyed("cpu.stat", "usage_usec").value_or(-1);
        usage.cpuThrottledUsec = keyed("cpu.stat", "throttled_usec").value_or(-1);
        usage.cpuPeriods = keyed("cpu.stat", "nr_periods").value_or(-1);
        usage.cpuThrottledPeriods = keyed("cpu.stat", "nr_throttled").value_or(-1);
        if (const auto text = readText(group / "cpu.max"); text.has_value()) usage.cpuLimit = ParseCpuMax(*text).value_or(-1.0);
        usage.tasks = single("pids.current");
        usage.tasksMax = single("pids.max");
        usage.tasksLimitHits = keyed("pids.events", "max").value_or(-1);
        usage.populated = keyed("cgroup.events", "populated").value_or(0) != 0;
        return usage;
    }

    std::string SafeName(const std::string_view name) {
        std::string safe;
        safe.reserve(name.size());
        for (const char c: name) {
            if (c == '/' || c == '\\' || static_cast<unsigned char>(c) < 0x20 || c == 0x7f) continue;
            safe.push_back(c);
        }
        // "." and ".." are directories already, and a leading dot is a hidden one nobody would
        // look for; none of them names an application.
        while (!safe.empty() && safe.front() == '.') safe.erase(safe.begin());
        return safe.empty() ? std::string("unnamed") : safe;
    }

    long long MemoryHighFor(const long long maxBytes, const int highPercent) {
        if (maxBytes <= 0 || highPercent <= 0 || highPercent >= 100) return -1;
        constexpr long long kPage = 4096;
        return (maxBytes / 100 * highPercent) / kPage * kPage;
    }

    std::optional<std::string> ApplyMemoryLimit(const std::filesystem::path &group, const long long maxBytes, const int highPercent) {
#ifdef _WIN32
        (void) group;
        (void) maxBytes;
        (void) highPercent;
        return std::string("cgroups exist on Linux only");
#else
        if (maxBytes <= 0) return std::string("a memory limit must be positive");

        // memory.high first: written after memory.max, there would be a moment with a ceiling and
        // no throttle - harmless before the process joins, but there is no reason to allow it.
        if (const auto high = MemoryHighFor(maxBytes, highPercent); high > 0) {
            if (const auto error = writeFile(group / "memory.high", std::to_string(high))) return error;
        }
        if (const auto error = writeFile(group / "memory.max", std::to_string(maxBytes))) return error;

        // Only where the kernel accounts swap per group - CONFIG_MEMCG_SWAP, and not switched off
        // with swapaccount=0. Without it there is no file, and nothing a group can be told.
        std::error_code ec;
        if (std::filesystem::exists(group / "memory.swap.max", ec)) {
            if (const auto error = writeFile(group / "memory.swap.max", "0")) return error;
        }
        return std::nullopt;
#endif
    }

    std::optional<std::string> ApplyCpuLimit(const std::filesystem::path &group, const double cores) {
#ifdef _WIN32
        (void) group;
        (void) cores;
        return std::string("cgroups exist on Linux only");
#else
        if (!(cores > 0.0)) return std::string("a CPU limit must be positive");
        // The kernel's default period, so a quota reads as what it is: 150000 of 100000 is one
        // and a half cores. 1 ms is the smallest quota the kernel takes.
        constexpr long long kPeriod = 100000;
        const auto quota = std::max(1000LL, static_cast<long long>(cores * static_cast<double>(kPeriod) + 0.5));
        return writeFile(group / "cpu.max", std::to_string(quota) + " " + std::to_string(kPeriod));
#endif
    }

    std::optional<std::string> ApplyTaskLimit(const std::filesystem::path &group, const long long tasks) {
#ifdef _WIN32
        (void) group;
        (void) tasks;
        return std::string("cgroups exist on Linux only");
#else
        if (tasks <= 0) return std::string("a task limit must be positive");
        return writeFile(group / "pids.max", std::to_string(tasks));
#endif
    }

    int OpenProcs(const std::filesystem::path &group) {
#ifdef _WIN32
        (void) group;
        return -1;
#else
        return ::open((group / "cgroup.procs").c_str(), O_WRONLY | O_CLOEXEC);
#endif
    }

    bool JoinFromChild(const int procsFd) noexcept {
#ifdef _WIN32
        (void) procsFd;
        return false;
#else
        if (procsFd < 0) return false;
        return ::write(procsFd, "0", 1) == 1;
#endif
    }

    Tree Tree::Setup(const std::filesystem::path &mount, std::optional<std::string> self) {

        Tree tree;

#ifdef _WIN32
        (void) mount;
        (void) self;
        tree._reason = "cgroups exist on Linux only";
        return tree;
#else
        if (!self.has_value()) {
            const auto text = readText("/proc/self/cgroup");
            if (!text.has_value()) {
                tree._reason = "this host has no /proc/self/cgroup, so no cgroups";
                return tree;
            }
            self = ParseSelfCgroup(*text);
            if (!self.has_value()) {
                tree._reason = "this host does not use the cgroup v2 hierarchy";
                return tree;
            }
        }

        const auto root = mount / *self;
        std::error_code ec;
        if (self->empty() || !std::filesystem::is_directory(root, ec)) {
            tree._reason = "this process's cgroup " + root.string() + " is not there";
            return tree;
        }

        // From here on the node's total can be read, delegated or not.
        tree._root = root;

        const auto controllers = readText(root / "cgroup.controllers").value_or("");
        if (!HasController(controllers, kRequired)) {
            tree._reason = "the memory controller is not available in " + root.string()
                           + " - add Delegate=memory cpu pids to the unit that starts this worker";
            return tree;
        }

        // The order is the kernel's, not a preference: a group with controllers enabled for its
        // children may hold no processes itself, so this process leaves the root before any
        // controller is enabled there. Making worker/ is also the test of whether the subtree was
        // delegated - without Delegate= the directory belongs to root.
        if (const auto error = makeDirectory(root / "worker")) {
            tree._reason = "the cgroup is not delegated to this user (" + *error
                           + ") - add Delegate=memory cpu pids to the unit that starts this worker";
            return tree;
        }
        if (const auto error = writeFile(root / "worker" / "cgroup.procs", std::to_string(::getpid()))) {
            tree._reason = "could not move this process into its own leaf group (" + *error + ")";
            return tree;
        }

        std::string enable = "+" + std::string(kRequired);
        for (const auto controller: kOptional) {
            if (HasController(controllers, controller)) enable += " +" + std::string(controller);
        }

        if (const auto error = writeFile(root / "cgroup.subtree_control", enable)) {
            tree._reason = "could not enable " + enable + " for the instances (" + *error + ")";
            return tree;
        }
        const auto applications = root / "applications";
        if (const auto error = makeDirectory(applications)) {
            tree._reason = "could not make " + applications.string() + " (" + *error + ")";
            return tree;
        }
        if (const auto error = writeFile(applications / "cgroup.subtree_control", enable)) {
            tree._reason = "could not enable " + enable + " below " + applications.string() + " (" + *error + ")";
            return tree;
        }

        tree._applications = applications;
        tree._enabled = true;
        tree._reason.clear();
        return tree;
#endif
    }

    std::optional<std::filesystem::path> Tree::CreateInstance(const std::string &application, const std::string &instanceId) const {
#ifdef _WIN32
        (void) application;
        (void) instanceId;
        return std::nullopt;
#else
        if (!_enabled) return std::nullopt;

        const auto applicationGroup = _applications / SafeName(application);
        if (makeDirectory(applicationGroup).has_value()) return std::nullopt;

        // Asked for every time rather than once per application, because it costs one write and
        // an application group that was swept and remade in between would otherwise have none.
        // What the parent enabled is what can be enabled here.
        if (const auto enabled = readText(_applications / "cgroup.subtree_control"); enabled.has_value() && !trim(*enabled).empty()) {
            std::string enable;
            for (std::size_t start = 0; start < enabled->size();) {
                auto end = enabled->find_first_of(" \n", start);
                if (end == std::string::npos) end = enabled->size();
                if (auto token = std::string_view(*enabled).substr(start, end - start); !token.empty()) {
                    if (token.front() == '+') token.remove_prefix(1);
                    if (!enable.empty()) enable += ' ';
                    enable += "+" + std::string(token);
                }
                start = end + 1;
            }
            std::ignore = writeFile(applicationGroup / "cgroup.subtree_control", enable);
        }

        const auto group = applicationGroup / SafeName(instanceId);
        if (makeDirectory(group).has_value()) return std::nullopt;
        return group;
#endif
    }

    bool Tree::RemoveInstance(const std::filesystem::path &group) {
#ifdef _WIN32
        (void) group;
        return false;
#else
        // rmdir, not remove_all: a cgroup directory is removed whole or not at all, and it refuses
        // while a process is still in it - which is the answer wanted.
        if (::rmdir(group.c_str()) != 0) return errno == ENOENT;
        // The application's group goes with its last instance. Refused while another instance is
        // there, which is fine.
        std::ignore = ::rmdir(group.parent_path().c_str());
        return true;
#endif
    }

    void Tree::Sweep(const std::function<bool(const std::string &)> &keep) const {
#ifndef _WIN32
        if (!_enabled) return;
        std::error_code ec;
        for (const auto &application: std::filesystem::directory_iterator(_applications, ec)) {
            if (!application.is_directory(ec)) continue;
            std::error_code inner;
            for (const auto &instance: std::filesystem::directory_iterator(application.path(), inner)) {
                if (!instance.is_directory(inner)) continue;
                if (keep(instance.path().filename().string())) continue;
                std::ignore = ::rmdir(instance.path().c_str());
            }
            std::ignore = ::rmdir(application.path().c_str());
        }
#else
        (void) keep;
#endif
    }

}// namespace Euclid::Core::Cgroup
