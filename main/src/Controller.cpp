// C++ includes
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ranges>
#include <thread>

// Boost includes
#include <boost/asio/io_context.hpp>
#include <boost/asio/local/stream_protocol.hpp>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/core/LogStream.h>
#include <euclid/database/Database.h>
#include <euclid/database/RepositoryFactory.h>
#include <euclid/dto/emm/EmmMapper.h>
#include <euclid/manager/Controller.h>
#include <euclid/manager/ControllerPlatform.h>

// ControllerPlatform.h pulls in <sys/wait.h>/<csignal>/<unistd.h>/... on POSIX, and
// <windows.h>/<process.h>/<io.h> on Windows - so the rest of this file only needs to
// branch on _WIN32 for the handful of calls (fork/exec vs CreateProcess, kill vs
// TerminateProcess, waitpid vs GetExitCodeProcess) that don't have a shared name.

namespace Euclid::main {

    // Escapes ASCII control bytes (other than tab) as \xHH before a child's output is forwarded
    // into the journal. A module's stdout/stderr isn't trusted content - it can carry raw bytes
    // from a misbehaving dependency (e.g. libmagic's own fprintf warnings when handed a
    // corrupt/mismatched magic database dump non-UTF8 garbage that happened to include a raw ESC
    // byte). journalctl -o cat prints the MESSAGE field completely unescaped, so an unsanitized
    // ESC byte here becomes an attacker-uncontrolled terminal escape sequence on whatever
    // terminal later views the log - which is exactly the kind of thing that can hang or crash a
    // terminal emulator. Every other log_raw()/log_* call in this codebase goes through
    // Boost.Log's own formatter, which already escapes control characters; this is the one path
    // that bypasses it by design (to preserve a child's own source location), so it needs its own
    // guard.
    static std::string sanitizeForLog(const std::string &raw) {
        std::string out;
        out.reserve(raw.size());
        for (const unsigned char c: raw) {
            if (c == '\t' || (c >= 0x20 && c != 0x7f)) {
                out += static_cast<char>(c);
            } else {
                char buf[5];
                std::snprintf(buf, sizeof(buf), "\\x%02x", c);
                out += buf;
            }
        }
        return out;
    }

    // Reads lines from fd until EOF. Re-emits each line through Boost.Log
    // using the child's own source location so it looks like a native log line.
    // Runs on a detached background thread; closes fd when done.
    static void drainPipe(const int fd, const bool isError) {
        std::string line;
        char ch;
        auto emit = [&](const std::string &raw) {
            log_raw(sanitizeForLog(raw));
        };
#if defined(_WIN32)
        while (Platform::PipeRead(fd, &ch, 1) == 1) {
#else
        while (read(fd, &ch, 1) == 1) {
#endif
            if (ch == '\n') {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                emit(line);
                line.clear();
            } else line += ch;
        }
        if (!line.empty()) emit(line);
#if defined(_WIN32)
        Platform::PipeClose(fd);
#else
        close(fd);
#endif
    }

    bool waitForSocket(const std::string &path, const int timeoutMs) {
        // Goes through boost::asio rather than raw BSD sockets so this works unchanged on
        // Windows (which only gained AF_UNIX support in Windows 10 1803+, via a different
        // header/API surface than POSIX) - boost::asio::local already abstracts that, and
        // UnixSocketServer's accept side uses the same protocol type.
        namespace local = boost::asio::local;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

        while (std::chrono::steady_clock::now() < deadline) {
            boost::asio::io_context ioc;
            local::stream_protocol::socket sock(ioc);
            boost::system::error_code ec;
            std::ignore = sock.connect(local::stream_protocol::endpoint(path), ec);
            if (!ec) return true;

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return false;
    }

    std::string makeInstanceSocketPath(const std::string &base, const pid_t pid) {
        const auto slash = base.find_last_of('/');
        if (const auto dot = base.find_last_of('.'); dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
            return base.substr(0, dot) + "." + std::to_string(pid) + base.substr(dot);
        }
        return base + "." + std::to_string(pid);
    }

    // Persists svc's current state as its slot's entry in its module's live instances array
    // (see database/include/euclid/database/entity/emm/Module.h), keyed by svc->instanceId so a
    // restart (which gets a new pid) updates the same entry in place rather than appending a new
    // one. Uses RepositoryFactory rather than a hardcoded backend so this respects
    // euclid.database.backend (mongodb or memory) like every other repository access.
    static void persistInstance(const std::shared_ptr<Dto::ModuleProcess> &svc) {
        Database::RepositoryFactory::instance().emmRepository()->upsertInstance(Dto::EmmMapper::toModuleEntity(*svc), Dto::EmmMapper::toInstanceEntity(*svc));
    }

#if defined(_WIN32)
    bool spawnInstance(const std::shared_ptr<Dto::ModuleProcess> &svc) {
        // Unlike fork()+exec(), CreateProcess() needs the full command line - including
        // --socket - before it hands back the new process's real pid, so the instance
        // socket path can't be derived from the pid the way the POSIX path does (there,
        // fork()'s return value in the parent and getpid() in the child are guaranteed to
        // be the same value, computable independently on both sides with no IPC). A
        // monotonic counter plays the same "unique id both sides agree on" role instead:
        // it's decided before spawning and passed to the child via --socket directly.
        static std::atomic<std::uint32_t> s_nextInstanceId{1};
        const auto instanceId = static_cast<pid_t>(s_nextInstanceId.fetch_add(1));
        const std::string instanceSocket = makeInstanceSocketPath(svc->config.socketPath, instanceId);

        pid_t pid = -1;
        HANDLE processHandle = nullptr;
        int outFd = -1, errFd = -1;
        if (!Platform::SpawnInstance(svc->config, instanceSocket, pid, processHandle, outFd, errFd)) {
            svc->state = Database::Entity::ModuleState::CRASHED;
            return false;
        }

        svc->pid = pid;
        svc->processHandle = processHandle;
        svc->instanceSocketPath = instanceSocket;
        svc->state = Database::Entity::ModuleState::STARTING;
        svc->startTime = std::chrono::steady_clock::now();
        svc->stdoutFd = outFd;
        svc->stderrFd = errFd;
        svc->activeRequests = 0;
        svc->lastIdleAt = std::chrono::steady_clock::now();

        std::thread(drainPipe, outFd, false).detach();
        std::thread(drainPipe, errFd, true).detach();

        if (waitForSocket(svc->instanceSocketPath, 5000)) {
            svc->state = Database::Entity::ModuleState::RUNNING;
            log_info << "Service ready, name: " << svc->config.name << ", pid: " << svc->pid << ", socket: " << svc->instanceSocketPath;
            persistInstance(svc);
            return true;
        }

        log_error << "Service " << svc->config.name << " did not become ready, killing pid " << svc->pid;
        Platform::ForceKill(svc->processHandle);
        ServiceController::waitForExit(svc->pid, 2000);
        if (svc->processHandle) {
            CloseHandle(svc->processHandle);
            svc->processHandle = nullptr;
        }
        if (!svc->instanceSocketPath.empty()) std::remove(svc->instanceSocketPath.c_str());
        svc->pid = -1;
        svc->instanceSocketPath.clear();
        svc->state = Database::Entity::ModuleState::PENDING_RESTART;
        svc->lastCrashTime = std::chrono::steady_clock::now();
        persistInstance(svc);
        return false;
    }
#else
    bool spawnInstance(const std::shared_ptr<Dto::ModuleProcess> &svc) {
        int outPipe[2], errPipe[2];
        std::ignore = pipe(outPipe);
        std::ignore = pipe(errPipe);

        const pid_t pid = fork();

        if (pid < 0) {
            svc->state = Database::Entity::ModuleState::CRASHED;
            return false;
        }

        if (pid == 0) {
            dup2(outPipe[1], STDOUT_FILENO);
            dup2(errPipe[1], STDERR_FILENO);

            close(outPipe[0]);
            close(outPipe[1]);
            close(errPipe[0]);
            close(errPipe[1]);

            setsid();

            // getpid() in the child returns exactly the value fork() returned to the parent, so
            // both sides can independently compute the same instance socket path with no IPC.
            const std::string instanceSocket = makeInstanceSocketPath(svc->config.socketPath, getpid());

            std::vector<const char *> argv;
            argv.push_back(svc->config.executable.c_str());
            for (auto &a: svc->config.args) argv.push_back(a.c_str());
            argv.push_back("--socket");
            argv.push_back(instanceSocket.c_str());
            argv.push_back(nullptr);

            execvp(svc->config.executable.c_str(), const_cast<char **>(argv.data()));
            _exit(127);
        }

        close(outPipe[1]);
        close(errPipe[1]);

        svc->pid = pid;
        svc->instanceSocketPath = makeInstanceSocketPath(svc->config.socketPath, pid);
        svc->state = Database::Entity::ModuleState::STARTING;
        svc->startTime = std::chrono::steady_clock::now();
        svc->stdoutFd = outPipe[0];
        svc->stderrFd = errPipe[0];
        svc->activeRequests = 0;
        svc->lastIdleAt = std::chrono::steady_clock::now();

        std::thread(drainPipe, outPipe[0], false).detach();
        std::thread(drainPipe, errPipe[0], true).detach();

        if (waitForSocket(svc->instanceSocketPath, 5000)) {
            svc->state = Database::Entity::ModuleState::RUNNING;
            log_info << "Service ready, name: " << svc->config.name << ", pid: " << svc->pid << ", socket: " << svc->instanceSocketPath;
            persistInstance(svc);
            return true;
        }

        log_error << "Service " << svc->config.name << " did not become ready, killing pid " << svc->pid;
        kill(svc->pid, SIGKILL);
        waitpid(svc->pid, nullptr, 0);
        if (!svc->instanceSocketPath.empty()) unlink(svc->instanceSocketPath.c_str());
        svc->pid = -1;
        svc->instanceSocketPath.clear();
        svc->state = Database::Entity::ModuleState::PENDING_RESTART;
        svc->lastCrashTime = std::chrono::steady_clock::now();
        persistInstance(svc);
        return false;
    }
#endif

    ServiceController::~ServiceController() {
        _running = false;
        if (_watchdog.joinable()) _watchdog.join();
    }

    void ServiceController::registerModule(const Dto::ModuleConfig &cfg) {
        std::lock_guard lock(_mutex);

        Dto::ModuleConfig normalized = cfg;
        normalized.minInstances = std::max(1, normalized.minInstances);
        normalized.maxInstances = std::max(normalized.minInstances, normalized.maxInstances);

        ServiceGroup group;
        group.config = normalized;
        for (int i = 0; i < normalized.minInstances; ++i) {
            auto svc = std::make_shared<Dto::ModuleProcess>();
            svc->config = normalized;
            group.instances.push_back(svc);
        }
        _services[normalized.name] = std::move(group);
    }

    bool ServiceController::start(const std::string &name) {
        std::vector<std::shared_ptr<Dto::ModuleProcess> > toSpawn;
        {
            std::lock_guard lock(_mutex);
            auto *group = getGroup(name);
            if (!group) return false;

            while (static_cast<int>(group->instances.size()) < group->config.minInstances) {
                auto svc = std::make_shared<Dto::ModuleProcess>();
                svc->config = group->config;
                group->instances.push_back(svc);
            }

            for (auto &svc: group->instances) {
                if (svc->state == Database::Entity::ModuleState::RUNNING || svc->state == Database::Entity::ModuleState::STARTING) continue;
                toSpawn.push_back(svc);
            }
        }

        bool ok = true;
        for (auto &svc: toSpawn) ok = spawnInstance(svc) && ok;
        return ok;
    }

    bool ServiceController::stop(const std::string &name, const int timeoutMs) {
        std::vector<std::shared_ptr<Dto::ModuleProcess> > toStop;
        {
            std::lock_guard lock(_mutex);
            const auto *group = getGroup(name);
            if (!group) return false;
            for (auto &svc: group->instances) if (svc->pid > 0) toStop.push_back(svc);
        }

        bool ok = true;
        for (auto &svc: toStop) ok = stopInstance(svc, timeoutMs) && ok;
        return ok;
    }

    bool ServiceController::restart(const std::string &name) {
        int restartDelayMs = 1000;
        {
            std::lock_guard lock(_mutex);
            if (const auto *group = getGroup(name)) restartDelayMs = group->config.restartDelayMs;
        }
        stop(name);
        std::this_thread::sleep_for(std::chrono::milliseconds(restartDelayMs));
        return start(name);
    }

    void ServiceController::startAll() {
        std::vector<std::string> names;
        {
            std::lock_guard lock(_mutex);
            for (const auto &name: _services | std::views::keys) names.push_back(name);
        }
        for (const auto &name: names) start(name);
    }

    void ServiceController::stopAll() {
        std::vector<std::string> names;
        {
            std::lock_guard lock(_mutex);
            for (const auto &name: _services | std::views::keys) names.push_back(name);
        }
        for (const auto &name: names) stop(name);
    }

    Database::Entity::ModuleState ServiceController::getState(const std::string &name) {
        std::lock_guard lock(_mutex);
        const auto *group = getGroup(name);
        if (!group || group->instances.empty()) return Database::Entity::ModuleState::STOPPED;

        bool anyRunning = false, anyTransitional = false;
        auto transitional = Database::Entity::ModuleState::STOPPED;
        for (const auto &svc: group->instances) {
            if (svc->state == Database::Entity::ModuleState::RUNNING) anyRunning = true;
            else if (svc->state != Database::Entity::ModuleState::STOPPED) {
                anyTransitional = true;
                transitional = svc->state;
            }
        }
        if (anyRunning) return Database::Entity::ModuleState::RUNNING;
        if (anyTransitional) return transitional;
        return Database::Entity::ModuleState::STOPPED;
    }

    std::vector<ServiceController::ModuleInstanceInfo> ServiceController::listModules() {
        std::lock_guard lock(_mutex);
        std::vector<ModuleInstanceInfo> result;
        for (const auto &group: _services | std::views::values) {
            for (const auto &svc: group.instances) {
                result.push_back({svc->config.name, svc->pid, svc->state, svc->instanceSocketPath, svc->activeRequests});
            }
        }
        return result;
    }

    std::optional<InstanceHandle> ServiceController::acquireInstance(const std::string &name) {
        std::lock_guard lock(_mutex);
        auto *group = getGroup(name);
        if (!group || group->instances.empty()) return std::nullopt;

        const size_t n = group->instances.size();
        for (size_t i = 0; i < n; ++i) {
            const size_t idx = (group->rrCursor + i) % n;
            if (const auto &svc = group->instances[idx]; svc->state == Database::Entity::ModuleState::RUNNING) {
                group->rrCursor = (idx + 1) % n;
                svc->activeRequests++;
                svc->wasBusySinceLastCheck = true;
                group->lastActivityAt = std::chrono::steady_clock::now();
                return InstanceHandle{.socketPath = svc->instanceSocketPath, .pid = svc->pid};
            }
        }
        return std::nullopt;
    }

    void ServiceController::releaseInstance(const std::string &name, const pid_t pid) {
        std::lock_guard lock(_mutex);
        const auto *group = getGroup(name);
        if (!group) return;
        for (auto &svc: group->instances) {
            if (svc->pid != pid) continue;
            if (svc->activeRequests > 0) svc->activeRequests--;
            if (svc->activeRequests == 0) svc->lastIdleAt = std::chrono::steady_clock::now();
            return;
        }
    }

    bool ServiceController::declareExpectedConcurrency(const std::string &name, const int desired) {
        std::lock_guard lock(_mutex);
        auto *group = getGroup(name);
        if (!group) return true;
        if (desired > group->config.maxInstances) return false;
        group->desiredCount = std::max(group->desiredCount, desired);
        return true;
    }

#if defined(_WIN32)
    bool ServiceController::waitForExit(const pid_t pid, const int timeoutMs) {
        HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
        if (!h) return true;// no such process = already gone

        const DWORD result = WaitForSingleObject(h, static_cast<DWORD>(timeoutMs));
        if (result == WAIT_OBJECT_0) {
            DWORD exitCode = 0;
            GetExitCodeProcess(h, &exitCode);
            log_info << "Process " << pid << " exited with code " << exitCode;
            CloseHandle(h);
            return true;
        }
        CloseHandle(h);
        return false;
    }
#else
    bool ServiceController::waitForExit(const pid_t pid, const int timeoutMs) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

        while (std::chrono::steady_clock::now() < deadline) {
            int status;
            const pid_t result = waitpid(pid, &status, WNOHANG);

            if (result == pid) {
                if (WIFEXITED(status)) {
                    log_info << "Process " << pid << " exited with code " << WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    log_info << "Process " << pid << " killed by signal " << WTERMSIG(status);
                }
                return true;
            }

            if (result == -1) {
                if (errno == ECHILD) return true;
                log_error << "Waitpid error: " << strerror(errno);
                return false;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        return false;
    }
#endif

    bool ServiceController::isDatabaseReachable() const {
        if (!_usesMongoBackend) return true;
        try {
            Database::Database::instance().ping();
            return true;
        } catch (const std::exception &) {
            return false;
        }
    }

    void ServiceController::startWatchdog() {
        _scaleDownIdleSeconds = Core::Configuration::instance().getOr<long>("euclid.scaling.scale-down-idle-seconds", 60);
        _usesMongoBackend = Core::Configuration::instance().getOr<std::string>("euclid.database.backend", "mongodb") != "memory";

        _watchdog = std::thread([this] {
            while (_running) {
                std::this_thread::sleep_for(std::chrono::seconds(1));

                // On POSIX this duplicates the SIGCHLD-driven call in main.cpp's dispatch
                // loop (harmless - waitpid(WNOHANG) just returns immediately when there's
                // nothing to reap). On Windows it's the only way instances get reaped at
                // all, since there's no SIGCHLD/waitpid(-1) equivalent there.
                onChildExit();

                const bool dbReachable = isDatabaseReachable();
                if (dbReachable != _databaseWasReachable) {
                    if (dbReachable)
                        log_info << "Database reachable again, resuming spawn/restart";
                    else
                        log_error << "Database unreachable - pausing all instance spawn/restart until it recovers";
                    _databaseWasReachable = dbReachable;
                }

                std::vector<std::shared_ptr<Dto::ModuleProcess> > toRestart;
                std::vector<std::shared_ptr<Dto::ModuleProcess> > toSpawn;
                std::vector<std::shared_ptr<Dto::ModuleProcess> > toStop;
                std::vector<std::shared_ptr<Dto::ModuleProcess> > toPersistGivenUp;
                if (dbReachable) {
                    std::lock_guard lock(_mutex);

                    for (auto &group: _services | std::views::values) {
                        for (auto &svc: group.instances) {
                            if (svc->state != Database::Entity::ModuleState::PENDING_RESTART) continue;

                            if (svc->config.maxRestarts != -1 && svc->restartCount >= svc->config.maxRestarts) {
                                log_error << "Instance " << svc->config.name << " exceeded max restarts (" << svc->config.maxRestarts << "), giving up";
                                svc->state = Database::Entity::ModuleState::STOPPED;
                                toPersistGivenUp.push_back(svc);
                                continue;
                            }

                            const auto now = std::chrono::steady_clock::now();
                            if (const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - svc->lastCrashTime).count(); elapsed < svc->currentDelay) {
                                log_debug << "Instance " << svc->config.name << " waiting for backoff (" << elapsed << "ms / " << svc->currentDelay << "ms)";
                                continue;
                            }

                            log_info << "Restarting " << svc->config.name << " (attempt " << svc->restartCount + 1 << ", delay was " << svc->currentDelay << "ms)";
                            svc->state = Database::Entity::ModuleState::RESTARTING;
                            svc->restartCount++;
                            svc->currentDelay = std::min(svc->currentDelay * 2, svc->config.maxRestartDelayMs);
                            toRestart.push_back(svc);
                        }
                    }

                    evaluateScaling(toSpawn, toStop);
                }

                // Persisted outside the lock (like every other DB write here) so a slow/unreachable
                // backend can't stall the watchdog's hold on _mutex, which acquireInstance() and
                // every other public method also need.
                for (auto &svc: toPersistGivenUp) persistInstance(svc);

                for (auto &svc: toRestart) {
                    if (const bool ok = spawnInstance(svc); !ok) {
                        log_error << "Instance " << svc->config.name << " failed to start";
                        svc->state = Database::Entity::ModuleState::PENDING_RESTART;
                        svc->lastCrashTime = std::chrono::steady_clock::now();
                    } else if (stableRuntime(svc) > 60) {
                        log_info << "Instance " << svc->config.name << " was stable, resetting backoff";
                        svc->restartCount = 0;
                        svc->currentDelay = svc->config.restartDelayMs;
                    }
                }

                for (auto &svc: toSpawn) {
                    log_info << "Scaling up " << svc->config.name << " (new instance)";
                    spawnInstance(svc);
                }

                for (auto &svc: toStop) {
                    log_info << "Scaling down " << svc->config.name << " (pid " << svc->pid << ", idle)";
                    stopInstance(svc);
                    // Unlike an ordinary stop() (which leaves the slot in the pool, just marked
                    // STOPPED, since it may still be restarted or is only stopping for a full
                    // manager shutdown), this instance was already erased from group.instances by
                    // evaluateScaling() above - it's permanently gone, not just idle - so its DB
                    // entry should be too, keeping the instances array a live mirror of the pool
                    // instead of accumulating scaled-down history forever.
                    Database::RepositoryFactory::instance().emmRepository()->removeInstance(svc->config.name, svc->instanceId);
                }
            }
        });
    }

    void ServiceController::handleExitedInstance(const std::shared_ptr<Dto::ModuleProcess> &svc) {
        if (svc->state == Database::Entity::ModuleState::STOPPING || svc->state == Database::Entity::ModuleState::STOPPED) {
            svc->pid = -1;
            svc->state = Database::Entity::ModuleState::STOPPED;
            persistInstance(svc);
            return;
        }

        const pid_t exitedPid = svc->pid;
        if (!svc->instanceSocketPath.empty()) std::remove(svc->instanceSocketPath.c_str());
        svc->pid = -1;
        svc->instanceSocketPath.clear();
        svc->state = Database::Entity::ModuleState::CRASHED;
        svc->lastCrashTime = std::chrono::steady_clock::now();
        persistInstance(svc);
        log_warning << "Service " << svc->config.name << " crashed (pid=" << exitedPid << ")";

        if (svc->config.autoRestart) scheduleRestart(svc);
    }

#if defined(_WIN32)
    void ServiceController::reapExitedWindows() {
        // Windows has no SIGCHLD/waitpid(-1) to learn about a child dying asynchronously,
        // so this polls every tracked instance's handle instead - called once per
        // watchdog tick (see startWatchdog()).
        std::vector<std::shared_ptr<Dto::ModuleProcess> > exited;
        {
            std::lock_guard lock(_mutex);
            for (auto &group: _services | std::views::values) {
                for (auto &svc: group.instances) {
                    if (svc->pid > 0 && !Platform::IsRunning(svc->processHandle)) {
                        exited.push_back(svc);
                    }
                }
            }
        }
        for (auto &svc: exited) {
            if (svc->processHandle) {
                CloseHandle(svc->processHandle);
                svc->processHandle = nullptr;
            }
            handleExitedInstance(svc);
        }
    }

    void ServiceController::onChildExit() { reapExitedWindows(); }
#else
    void ServiceController::onChildExit() {
        int status;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            auto svc = findByPid(pid);
            if (!svc) continue;
            handleExitedInstance(svc);
        }
    }
#endif

    void ServiceController::restartAll() {
        std::vector<std::string> names;
        {
            std::lock_guard lock(_mutex);
            for (const auto &name: _services | std::views::keys) names.push_back(name);
        }
        for (const auto &name: names) restart(name);
    }

    ServiceController::ServiceGroup *ServiceController::getGroup(const std::string &name) {
        const auto it = _services.find(name);
        if (it == _services.end()) return nullptr;
        return &it->second;
    }

    std::shared_ptr<Dto::ModuleProcess> ServiceController::findByPid(const pid_t pid) {
        std::lock_guard lock(_mutex);
        for (auto &group: _services | std::views::values) for (auto &svc: group.instances) if (svc->pid == pid) return svc;
        return nullptr;
    }

    long ServiceController::stableRuntime(const std::shared_ptr<Dto::ModuleProcess> &svc) {
        const auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::seconds>(now - svc->startTime).count();
    }

    void ServiceController::scheduleRestart(const std::shared_ptr<Dto::ModuleProcess> &svc) {
        svc->lastCrashTime = std::chrono::steady_clock::now();
        svc->state = Database::Entity::ModuleState::CRASHED;
    }

    bool ServiceController::stopInstance(const std::shared_ptr<Dto::ModuleProcess> &svc, const int timeoutMs) {
        if (!svc || svc->pid <= 0) return false;

        svc->state = Database::Entity::ModuleState::STOPPING;
        log_info << "Stopping instance '" << svc->config.name << "' (pid " << svc->pid << ")";

#if defined(_WIN32)
        Platform::RequestGracefulStop(svc->pid);
        if (!waitForExit(svc->pid, timeoutMs)) {
            log_warning << "Instance '" << svc->config.name << "' (pid " << svc->pid << ") did not stop in " << timeoutMs << "ms, terminating";
            Platform::ForceKill(svc->processHandle);
            if (!waitForExit(svc->pid, 2000)) {
                log_error << "ERROR: Instance '" << svc->config.name << "' (pid " << svc->pid << ") could not be killed";
                persistInstance(svc);
                return false;
            }
            log_info << "Instance '" << svc->config.name << "' (pid " << svc->pid << ") killed";
        } else {
            log_info << "Instance '" << svc->config.name << "' (pid " << svc->pid << ") stopped cleanly";
        }
        if (svc->processHandle) {
            CloseHandle(svc->processHandle);
            svc->processHandle = nullptr;
        }
#else
        kill(svc->pid, SIGTERM);
        if (!waitForExit(svc->pid, timeoutMs)) {
            log_warning << "Instance '" << svc->config.name << "' (pid " << svc->pid << ") did not stop in " << timeoutMs << "ms, sending SIGKILL";
            kill(svc->pid, SIGKILL);
            if (!waitForExit(svc->pid, 2000)) {
                log_error << "ERROR: Instance '" << svc->config.name << "' (pid " << svc->pid << ") could not be killed";
                persistInstance(svc);
                return false;
            }
            log_info << "Instance '" << svc->config.name << "' (pid " << svc->pid << ") killed";
        } else {
            log_info << "Instance '" << svc->config.name << "' (pid " << svc->pid << ") stopped cleanly";
        }
#endif

        if (!svc->instanceSocketPath.empty()) std::remove(svc->instanceSocketPath.c_str());
        svc->pid = -1;
        svc->instanceSocketPath.clear();
        svc->state = Database::Entity::ModuleState::STOPPED;
        persistInstance(svc);
        return true;
    }

    void ServiceController::evaluateScaling(std::vector<std::shared_ptr<Dto::ModuleProcess> > &toSpawn,
                                            std::vector<std::shared_ptr<Dto::ModuleProcess> > &toStop) {
        const auto now = std::chrono::steady_clock::now();

        for (auto &group: _services | std::views::values) {
            int running = 0, busy = 0;
            std::shared_ptr<Dto::ModuleProcess> idleCandidate;

            for (const auto &svc: group.instances) {
                if (svc->state != Database::Entity::ModuleState::RUNNING) continue;
                ++running;
                // wasBusySinceLastCheck catches bursts that happened between two ticks, not just
                // ones caught mid-flight at this exact instant - see the field's doc comment.
                if (svc->activeRequests > 0 || svc->wasBusySinceLastCheck) {
                    ++busy;
                } else if (!idleCandidate || svc->lastIdleAt < idleCandidate->lastIdleAt) {
                    // Least-recently-used among the currently-idle instances, so if the group
                    // does turn out to be idle enough to shrink, we stop the one round-robin has
                    // favored least rather than an arbitrary one.
                    idleCandidate = svc;
                }
                svc->wasBusySinceLastCheck = false;
            }

            // Scale up on ANY current demand rather than requiring every instance to be busy at
            // once: acquireInstance() round-robins blindly (it doesn't prefer idle instances), so
            // one instance being busy is already a sign the pool is under load, not proof the
            // whole group is saturated. Requiring full saturation at a single 1s watchdog tick
            // made this almost never fire for a workload of many quick requests - throughput would
            // stay well above what one instance could serve, but the poll would rarely catch every
            // instance mid-request at the same instant.
            // running < desiredCount fires scale-up proactively, ahead of any busy sample, when a
            // client has declared it's about to need more instances than the pool currently has -
            // see declareExpectedConcurrency()'s doc comment for why busy>0 alone can't be trusted
            // to catch this for high-instance-count/short-request workloads.
            if ((busy > 0 || running < group.desiredCount) && running < group.config.maxInstances) {
                auto svc = std::make_shared<Dto::ModuleProcess>();
                svc->config = group.config;
                group.instances.push_back(svc);
                toSpawn.push_back(svc);
            } else if (idleCandidate && running > group.config.minInstances &&
                       std::chrono::duration_cast<std::chrono::seconds>(now - group.lastActivityAt).count() >= _scaleDownIdleSeconds) {
                // Gated on the GROUP's last activity, not just this instance's: round-robin can
                // leave one instance unpicked for a while even while its siblings stay busy, and
                // per-instance idle time alone can't tell "nobody wants this instance" apart from
                // "nobody wants any instance" - the former should NOT trigger a scale-down while
                // the group is still doing real work.
                std::erase(group.instances, idleCandidate);
                toStop.push_back(idleCandidate);
                // The group is genuinely idle now, so drop any earlier declared target back to the
                // floor - otherwise a one-off high-concurrency declaration would keep forcing the
                // pool back up forever even after that workload finished.
                group.desiredCount = group.config.minInstances;
            }
        }
    }

}// namespace Euclid::main