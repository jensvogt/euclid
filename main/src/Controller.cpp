// C++ includes
#include <algorithm>
#include <csignal>
#include <cstring>
#include <ranges>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/core/LogStream.h>
#include <euclid/database/repository/module/MongoModuleRepository.h>
#include <euclid/dto/module/ModuleMapper.h>
#include <euclid/manager/Controller.h>
#include <euclid/manager/ControllerPlatform.h>

namespace Euclid::main {

    // Reads lines from fd until EOF. Re-emits each line through Boost.Log
    // using the child's own source location so it looks like a native log line.
    // Runs on a detached background thread; closes fd when done.
    static void drainPipe(const int fd, const bool isError) {
        std::string line;
        char ch;
        auto emit = [&](const std::string &raw) {
            log_raw(raw);
        };
        while (read(fd, &ch, 1) == 1) {
            if (ch == '\n') {
                emit(line);
                line.clear();
            } else line += ch;
        }
        if (!line.empty()) emit(line);
        close(fd);
    }

    bool waitForSocket(const std::string &path, const int timeoutMs) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

        while (std::chrono::steady_clock::now() < deadline) {
            const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

            if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
                close(fd);
                return true;
            }

            close(fd);
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
            Database::MongoModuleRepository::instance().upsert(Dto::ModuleMapper::toEntity(*svc));
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
        Database::MongoModuleRepository::instance().upsert(Dto::ModuleMapper::toEntity(*svc));
        return false;
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

    void ServiceController::startWatchdog() {
        _scaleDownIdleSeconds = Core::Configuration::instance().getOr<long>("euclid.scaling.scale-down-idle-seconds", 60);

        _watchdog = std::thread([this] {
            while (_running) {
                std::this_thread::sleep_for(std::chrono::seconds(1));

                std::vector<std::shared_ptr<Dto::ModuleProcess> > toRestart;
                std::vector<std::shared_ptr<Dto::ModuleProcess> > toSpawn;
                std::vector<std::shared_ptr<Dto::ModuleProcess> > toStop;
                {
                    std::lock_guard lock(_mutex);

                    for (auto &group: _services | std::views::values) {
                        for (auto &svc: group.instances) {
                            if (svc->state != Database::Entity::ModuleState::PENDING_RESTART) continue;

                            if (svc->config.maxRestarts != -1 && svc->restartCount >= svc->config.maxRestarts) {
                                log_error << "Instance " << svc->config.name << " exceeded max restarts (" << svc->config.maxRestarts << "), giving up";
                                svc->state = Database::Entity::ModuleState::STOPPED;
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
                }
            }
        });
    }

    void ServiceController::onChildExit() {
        int status;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            auto svc = findByPid(pid);
            if (!svc) continue;

            if (svc->state == Database::Entity::ModuleState::STOPPING || svc->state == Database::Entity::ModuleState::STOPPED) {
                svc->pid = -1;
                svc->state = Database::Entity::ModuleState::STOPPED;
                Database::MongoModuleRepository::instance().upsert(Dto::ModuleMapper::toEntity(*svc));
                continue;
            }

            if (!svc->instanceSocketPath.empty()) unlink(svc->instanceSocketPath.c_str());
            svc->pid = -1;
            svc->instanceSocketPath.clear();
            svc->state = Database::Entity::ModuleState::CRASHED;
            svc->lastCrashTime = std::chrono::steady_clock::now();
            Database::MongoModuleRepository::instance().upsert(Dto::ModuleMapper::toEntity(*svc));
            log_warning << "Service " << svc->config.name << " crashed (pid=" << pid << ")";

            if (svc->config.autoRestart) scheduleRestart(svc);
        }
    }

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

        kill(svc->pid, SIGTERM);
        if (!waitForExit(svc->pid, timeoutMs)) {
            log_warning << "Instance '" << svc->config.name << "' (pid " << svc->pid << ") did not stop in " << timeoutMs << "ms, sending SIGKILL";
            kill(svc->pid, SIGKILL);
            if (!waitForExit(svc->pid, 2000)) {
                log_error << "ERROR: Instance '" << svc->config.name << "' (pid " << svc->pid << ") could not be killed";
                Database::MongoModuleRepository::instance().upsert(Dto::ModuleMapper::toEntity(*svc));
                return false;
            }
            log_info << "Instance '" << svc->config.name << "' (pid " << svc->pid << ") killed";
        } else {
            log_info << "Instance '" << svc->config.name << "' (pid " << svc->pid << ") stopped cleanly";
        }

        if (!svc->instanceSocketPath.empty()) unlink(svc->instanceSocketPath.c_str());
        svc->pid = -1;
        svc->instanceSocketPath.clear();
        svc->state = Database::Entity::ModuleState::STOPPED;
        Database::MongoModuleRepository::instance().upsert(Dto::ModuleMapper::toEntity(*svc));
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
                if (svc->activeRequests > 0) {
                    ++busy;
                } else if (!idleCandidate || svc->lastIdleAt < idleCandidate->lastIdleAt) {
                    // Least-recently-used among the currently-idle instances, so if the group
                    // does turn out to be idle enough to shrink, we stop the one round-robin has
                    // favored least rather than an arbitrary one.
                    idleCandidate = svc;
                }
            }

            // Scale up on ANY current demand rather than requiring every instance to be busy at
            // once: acquireInstance() round-robins blindly (it doesn't prefer idle instances), so
            // one instance being busy is already a sign the pool is under load, not proof the
            // whole group is saturated. Requiring full saturation at a single 1s watchdog tick
            // made this almost never fire for a workload of many quick requests - throughput would
            // stay well above what one instance could serve, but the poll would rarely catch every
            // instance mid-request at the same instant.
            if (busy > 0 && running < group.config.maxInstances) {
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
            }
        }
    }

}// namespace Euclid::main