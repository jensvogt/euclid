// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <sstream>
#include <set>
#include <thread>

// Boost includes
#include <boost/asio/io_context.hpp>
#include <boost/json.hpp>
#include <boost/asio/local/stream_protocol.hpp>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <Placement.h>
#include <euclid/core/ApplicationLaunch.h>
#include <euclid/core/ArtifactFetcher.h>
#include <euclid/core/CryptoUtils.h>
#include <euclid/core/DateTimeUtils.h>
#include <euclid/core/DirUtils.h>
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/JwtUtils.h>
#include <euclid/core/LogStream.h>
#include <euclid/database/entity/RuntimeName.h>
#include <euclid/database/entity/eap/ApplicationIdentity.h>
#include <euclid/database/Database.h>
#include <euclid/database/RepositoryFactory.h>
#include <euclid/database/entity/ets/TransferServer.h>
#include <euclid/dto/emm/EmmMapper.h>
#include <euclid/manager/ApplicationCleanup.h>
#include <euclid/manager/BacklogTarget.h>
#include <euclid/manager/UtilisationSignals.h>
#include <euclid/manager/Controller.h>
#include <euclid/manager/ControllerPlatform.h>
#include <euclid/manager/StartOrder.h>

// ControllerPlatform.h pulls in <sys/wait.h>/<csignal>/<unistd.h>/ on POSIX, and
// <windows.h>/<process.h>/<io.h> on Windows - so the rest of this file only needs to
// branch on _WIN32 for the handful of calls (fork/exec vs CreateProcess, kill vs
// TerminateProcess, waitpid vs GetExitCodeProcess) that don't have a shared name.

#if defined(__linux__)
// PR_SET_PDEATHSIG, which is how a spawned instance is made to die with the manager. Linux only:
// macOS has no equivalent, so an instance there outlives a manager that was killed - see the
// comment at the call.
#include <sys/prctl.h>
#endif

namespace Euclid::main {

    // The API gateway module. Named here because the shutdown ordering has to treat it as a
    // client of the applications rather than as one of the modules they depend on - see
    // stopOrder().
    constexpr auto kApiGateway = "eag";

    // The channel a spawned process's own output is logged on. An application and a euclid module
    // are told apart because they are turned down for quite different reasons: an application is
    // somebody else's program and its output is theirs, while a module's is euclid's own. An
    // application's is Core::Launch's, the channel a worker uses for the same application.
    static std::string outputChannel(const Dto::ModuleConfig &config) {
        return config.application ? Core::Launch::OutputChannel(config.name)
                                  : std::string(Core::LogStream::kModuleChannel) + "." + config.name;
    }

    // What euclid knows about the process a line came from and the line itself does not say - see
    // Core::Launch::OutputFields. Empty for a euclid module, which belongs to no namespace in
    // particular.
    static std::string outputFields(const Dto::ModuleConfig &config) {
        if (!config.application) return {};
        return Core::Launch::OutputFields(config.name, config.nameSpace, config.accountId);
    }

    // Reads lines from fd until EOF, re-emitting each on the channel of the process it came from -
    // "app.<name>" for an application, "module.<name>" for a euclid module. That channel is what
    // makes this output something an operator can turn down or off on its own
    // (euclid.logging.channels), which matters because a single talkative application otherwise
    // buries everything euclid itself has to say. The line is written verbatim, as it always was:
    // it arrives already formatted by whatever wrote it, and stderr is recorded as an error so a
    // channel left at "error" still shows what went wrong.
    //
    // Runs on a detached background thread; closes fd when done.
    static void drainPipe(const int fd, const bool isError, const std::string channel, const std::string fields) {
        std::string line;
        char ch;
        // Sanitised, and recorded at the severity the program itself wrote where it said one - see
        // Core::Launch::EmitOutput, which a worker records its instances' output with too.
        auto emit = [&](const std::string &raw) { Core::Launch::EmitOutput(channel, raw, isError, fields); };
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

    // Goes through boost::asio rather than raw BSD sockets so this works unchanged on
    // Windows (which only gained AF_UNIX support in Windows 10 1803+, via a different
    // header/API surface than POSIX) - boost::asio::local already abstracts that, and
    // UnixSocketServer's accept side uses the same protocol type.
    // Whether a process is still running after graceMs, which is what "started" means for a
    // program the manager only supervises rather than routes to.
#if defined(_WIN32)
    bool waitAlive(HANDLE processHandle, const int graceMs) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(graceMs);
        while (std::chrono::steady_clock::now() < deadline) {
            if (!Platform::IsRunning(processHandle)) {
                log_error << "Process exited during startup";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return true;
    }
#else
    // Watches the instance record rather than calling waitpid(): onChildExit() reaps every child
    // from the SIGCHLD handler, so a second waiter here would race it and lose - waitpid() on an
    // already-reaped child returns -1/ECHILD, which reads exactly like "still running" and had a
    // program that died on startup reported as ready. handleExitedInstance() clears the pid when
    // it reaps, so that is the fact to watch.
    bool waitAlive(const std::shared_ptr<Dto::ModuleProcess> &svc, const int graceMs) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(graceMs);
        while (std::chrono::steady_clock::now() < deadline) {
            if (svc->pid <= 0) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return svc->pid > 0;
    }
#endif

    bool waitForSocket(const std::string &path, const int timeoutMs) {
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

    // Ports handed out to instances, by instance id.
    //
    // File-scope because spawnInstance() is a free function with no view of a pool, and the check
    // that matters is "is any live instance already on this port" rather than anything a single
    // pool knows. One map covers every application at once, which is also what makes two
    // applications unable to collide with each other.
    std::mutex &httpPortsMutex() {
        static std::mutex mutex;
        return mutex;
    }

    std::map<std::string, int> &httpPorts() {
        static std::map<std::string, int> ports;
        return ports;
    }

    // A TCP port for one instance's own HTTP listener, or 0 when the installation has not set a
    // range aside for applications.
    //
    // Applications are the only pools that need this. A euclid module talks over the Unix socket
    // the manager gives it and binds nothing; an application may serve a web interface of its own,
    // and several instances of it share this host - so a port named in the application's own
    // configuration is bound by whichever instance starts first and refused to all the others.
    // That failure is invisible to the autoscaler: it sees a pool that will not grow, and restarts
    // the same instance until it gives up.
    //
    // Assigned from a configured range rather than left to the operating system, because the port
    // has to be knowable afterwards: an API gateway in front of the application needs to find its
    // backends, and a port the kernel picked is written down nowhere.
    int allocateHttpPort(const std::shared_ptr<Dto::ModuleProcess> &svc) {

        const auto &configuration = Core::Configuration::instance();
        const auto first = configuration.getOr<long>("euclid.modules.eap.http-port-min", 0);
        const auto last = configuration.getOr<long>("euclid.modules.eap.http-port-max", 0);
        if (first <= 0 || last < first) return 0;

        std::lock_guard lock(httpPortsMutex());

        // A restart of the same slot keeps the port it had, so anything pointed at this instance
        // does not have to be told about a restart it never noticed. The rule is Core::Launch's,
        // the one a worker gives its own instances ports by.
        const auto port = Core::Launch::PickHttpPort(httpPorts(), svc->instanceId, first, last);
        if (port == 0) {
            // Every port in the range is spoken for. Nothing is invented outside it: a port the
            // operator did not set aside might belong to something else on this host.
            log_warning << "No free HTTP port for " << svc->config.name << " in " << first << "-" << last;
            return 0;
        }
        httpPorts()[svc->instanceId] = port;
        return port;
    }

    // Gives an instance's port back when the slot itself is gone, not when it merely stopped: a
    // stopped slot is restarted into the same port, and handing it to somebody else meanwhile is
    // how two instances end up fighting over one.
    void releaseHttpPort(const std::string &instanceId) {
        std::lock_guard lock(httpPortsMutex());
        httpPorts().erase(instanceId);
    }

    // Persists svc's current state as its slot's entry in its module's live instances array
    // (see database/include/euclid/database/entity/emm/Module.h), keyed by svc->instanceId so a
    // restart (which gets a new pid) updates the same entry in place rather than appending a new
    // one. Uses RepositoryFactory rather than a hardcoded backend so this respects
    // euclid.database.backend (mongodb or memory) like every other repository access.
    static void persistInstance(const std::shared_ptr<Dto::ModuleProcess> &svc) {
        Database::RepositoryFactory::instance().emmRepository()->upsertInstance(Dto::EmmMapper::toModuleEntity(*svc), Dto::EmmMapper::toInstanceEntity(*svc));
    }

    // What a process exited with, in words, for the crash line below. The two codes the child
    // itself chooses are the ones worth naming: the forked child calls execvp() and _exit(127) if
    // it returns, and _exit(126) if it could not enter the working directory first - so both are
    // failures that happen before the process can write a syllable of its own to stderr.
    //
    // Which is exactly how a wrong runtime path presents: three lines saying an application
    // crashed and failed to start, and nothing anywhere naming the executable that does not exist.
    static std::string describeExit(const int status, const std::string &executable, const std::string &workingDir) {

        if (status < 0) return {};

#ifndef _WIN32
        if (WIFSIGNALED(status)) return ", killed by signal " + std::to_string(WTERMSIG(status));
        if (!WIFEXITED(status)) return {};
        const int code = WEXITSTATUS(status);
#else
        const int code = status;
#endif

        if (code == 127) {
            return ", exit code 127 - the executable could not be run, which is usually that it is"
                   " not there: " + executable;
        }
        if (code == 126) {
            return ", exit code 126 - the working directory could not be entered: " + workingDir;
        }
        return ", exit code " + std::to_string(code);
    }

    // Whether this module is the one holding the store the manager reads from - which is what
    // euclid.database.backend names when it does not name a database. False on MongoDB and on the
    // in-process store, neither of which is a module this manager starts.
    //
    // The one place the manager cannot treat a module as a module: anything it would write about
    // this one has to go through the very process it is about to start.
    static bool holdsTheStore(const std::string &name) {
        return !name.empty() && Core::Configuration::instance().getOr<std::string>("euclid.database.backend", "mongodb") == name;
    }

#if defined(_WIN32)
    // Both of the kernel handles an instance owns, released together because they have the same
    // lifetime - the process is gone, so neither the handle to it nor the event used to ask it
    // to stop is good for anything. One function rather than the pair open-coded at each of the
    // three places an instance ends, so a fourth cannot quietly leak the event the way it would
    // have to be remembered separately otherwise.
    static void closeInstanceHandles(const std::shared_ptr<Dto::ModuleProcess> &svc) {
        if (svc->processHandle) {
            CloseHandle(svc->processHandle);
            svc->processHandle = nullptr;
        }
        if (svc->stopEvent) {
            CloseHandle(svc->stopEvent);
            svc->stopEvent = nullptr;
        }
    }
#endif

#if defined(_WIN32)
    bool spawnInstance(const std::shared_ptr<Dto::ModuleProcess> &svc) {

        // Which pool slot this process is, so that anything it reports about itself can be
        // matched back to the instance the manager would start or stop. instanceId is the right
        // identifier rather than the pid: it is stable across restarts of the same slot, and it
        // is what emm_module.instances[] is already keyed by. Set on the instance's own copy of
        // the config - each ModuleProcess holds one - so both spawn paths pick it up from the
        // same place.
        svc->config.environment["EUCLID_INSTANCE_ID"] = svc->instanceId;

        // And forget what the last process in this slot said about itself, before the next one is
        // in a position to say anything. instanceId is stable across restarts, so those figures
        // are inherited otherwise - and backgroundTasks in particular can never correct itself:
        // the count lives in the process, and a process that was stopped mid---async never counted
        // down. An instance carrying a phantom count is one evaluateScaling() will not scale down
        // again, for the life of the installation.
        svc->backgroundTasks = 0;

        // Every module but the one holding the store. That one's reports lived inside the process
        // that has just gone, and its store went with them - so there is nothing left to clear, and
        // the attempt is made at the one moment it cannot succeed: before the replacement is
        // listening. It failed on every single start, in a warning that read as though the store
        // were broken rather than not up yet.
        if (!holdsTheStore(svc->config.name)) {
            Database::RepositoryFactory::instance().emmRepository()->clearInstanceReports(svc->config.name, svc->instanceId);
        }

        // Only applications are given one - see allocateHttpPort(). An application binds it with
        // something like server.port=${EUCLID_HTTP_PORT:8080}, so the same artifact still runs
        // outside euclid on its own default.
        if (svc->config.application) {
            if (const auto port = allocateHttpPort(svc); port > 0) {
                svc->httpPort = port;
                svc->config.environment["EUCLID_HTTP_PORT"] = std::to_string(port);
            }
        }
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
        HANDLE processHandle = nullptr, stopEvent = nullptr;
        int outFd = -1, errFd = -1;
        if (!Platform::SpawnInstance(svc->config, instanceSocket, pid, processHandle, stopEvent, outFd, errFd)) {
            // Marked and reported. Platform::SpawnInstance() has already said what Windows
            // answered; this says which instance it was about, so a pool that never started can be
            // told from one that started and died - which without these two lines looked the same
            // from the log, both of them being nothing at all.
            log_error << "Instance " << svc->config.name << " could not be spawned, executable: " << svc->config.executable;
            svc->state = Database::Entity::ModuleState::CRASHED;
            return false;
        }

        svc->pid = pid;
        svc->processHandle = processHandle;
        svc->stopEvent = stopEvent;
        svc->instanceSocketPath = instanceSocket;
        svc->state = Database::Entity::ModuleState::STARTING;
        svc->startTime = std::chrono::steady_clock::now();
        svc->stdoutFd = outFd;
        svc->stderrFd = errFd;
        svc->activeRequests = 0;
        svc->inFlightRequests = 0;
        svc->lastIdleAt = std::chrono::steady_clock::now();

        const auto channel = outputChannel(svc->config);
        const auto fields = outputFields(svc->config);
        std::thread(drainPipe, outFd, false, channel, fields).detach();
        std::thread(drainPipe, errFd, true, channel, fields).detach();

        const bool liveness = svc->config.readiness == Dto::ModuleConfig::ReadinessCheck::Liveness;
        if (liveness
                ? waitAlive(svc->processHandle, svc->config.livenessGraceMs)
                : waitForSocket(svc->instanceSocketPath, svc->config.readyTimeoutMs)) {
            svc->state = Database::Entity::ModuleState::RUNNING;
            log_info << "Service ready, name: " << svc->config.name << ", pid: " << svc->pid
                    << (liveness ? ", running" : ", socket: " + svc->instanceSocketPath);
            persistInstance(svc);
            return true;
        }

        log_error << "Service " << svc->config.name
                << (liveness ? " exited during startup, pid " : " did not become ready, killing pid ") << svc->pid;
        Platform::ForceKill(svc->processHandle);
        ServiceController::waitForExit(svc->pid, 2000);
        closeInstanceHandles(svc);
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

        // Which pool slot this process is, so that anything it reports about itself can be
        // matched back to the instance the manager would start or stop. instanceId is the right
        // identifier rather than the pid: it is stable across restarts of the same slot, and it
        // is what emm_module.instances[] is already keyed by. Set on the instance's own copy of
        // the config - each ModuleProcess holds one - so both spawn paths pick it up from the
        // same place.
        svc->config.environment["EUCLID_INSTANCE_ID"] = svc->instanceId;

        // And forget what the last process in this slot said about itself, before the next one is
        // in a position to say anything. instanceId is stable across restarts, so those figures
        // are inherited otherwise - and backgroundTasks in particular can never correct itself:
        // the count lives in the process, and a process that was stopped mid---async never counted
        // down. An instance carrying a phantom count is one evaluateScaling() will not scale down
        // again, for the life of the installation.
        svc->backgroundTasks = 0;

        // Every module but the one holding the store. That one's reports lived inside the process
        // that has just gone, and its store went with them - so there is nothing left to clear, and
        // the attempt is made at the one moment it cannot succeed: before the replacement is
        // listening. It failed on every single start, in a warning that read as though the store
        // were broken rather than not up yet.
        if (!holdsTheStore(svc->config.name)) {
            Database::RepositoryFactory::instance().emmRepository()->clearInstanceReports(svc->config.name, svc->instanceId);
        }

        // Only applications are given one - see allocateHttpPort(). An application binds it with
        // something like server.port=${EUCLID_HTTP_PORT:8080}, so the same artifact still runs
        // outside euclid on its own default.
        if (svc->config.application) {
            if (const auto port = allocateHttpPort(svc); port > 0) {
                svc->httpPort = port;
                svc->config.environment["EUCLID_HTTP_PORT"] = std::to_string(port);
            }
        }
        int outPipe[2], errPipe[2];
        std::ignore = pipe(outPipe);
        std::ignore = pipe(errPipe);

        // Read before the fork, because afterwards the child has no other way to ask who its
        // parent was: getppid() answers truthfully only while that parent is still alive, and a
        // parent that is not is exactly the case the child has to tell apart.
        //
        // maybe_unused for macOS, which has no PR_SET_PDEATHSIG and therefore nothing to compare
        // this against.
        [[maybe_unused]] const pid_t parentPid = getpid();

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

#if defined(__linux__)
            // Die with the manager, whatever happens to it. A manager that is killed - or that
            // crashes, or is stopped the hard way - otherwise leaves its children running: they go
            // on consuming the queues their application listens to, holding the HTTP port the next
            // instance will be handed, and reporting their load to a manager that has no record of
            // them. The Windows side gets the same guarantee from a kill-on-close job object; see
            // Platform::SpawnInstance().
            //
            // Set before setsid() and before exec, so the window in which it does not apply is as
            // small as it can be made. It survives exec, which is the point - the application
            // euclid runs is not euclid's code and cannot be asked to arrange this for itself.
            prctl(PR_SET_PDEATHSIG, SIGKILL);

            // And the race it cannot close: the manager can die between fork() and the line above,
            // in which case the signal was armed against a parent that is already gone and will
            // never arrive. Being reparented to init is what that looks like from here.
            if (getppid() != parentPid) _exit(128 + SIGKILL);
#endif

            setsid();

            // getpid() in the child returns exactly the value fork() returned to the parent, so
            // both sides can independently compute the same instance socket path with no IPC.
            const std::string instanceSocket = makeInstanceSocketPath(svc->config.socketPath, getpid());

            if (!svc->config.workingDir.empty()) {
                if (chdir(svc->config.workingDir.c_str()) != 0) _exit(126);
            }

            for (const auto &[name, value]: svc->config.environment) {
                setenv(name.c_str(), value.c_str(), 1);
            }

            // The socket also travels as an environment variable, not only as --socket: an
            // application written in some other language has no reason to understand euclid's
            // command line, but every runtime can read its environment.
            setenv("EUCLID_SOCKET", instanceSocket.c_str(), 1);

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
        svc->inFlightRequests = 0;
        svc->lastIdleAt = std::chrono::steady_clock::now();

        const auto channel = outputChannel(svc->config);
        const auto fields = outputFields(svc->config);
        std::thread(drainPipe, outPipe[0], false, channel, fields).detach();
        std::thread(drainPipe, errPipe[0], true, channel, fields).detach();

        // An application is judged by whether it is still running, a module by whether it has
        // created its socket - see ModuleConfig::ReadinessCheck for why the two differ.
        const bool liveness = svc->config.readiness == Dto::ModuleConfig::ReadinessCheck::Liveness;
        if (liveness
                ? waitAlive(svc, svc->config.livenessGraceMs)
                : waitForSocket(svc->instanceSocketPath, svc->config.readyTimeoutMs)) {
            svc->state = Database::Entity::ModuleState::RUNNING;
            log_info << "Service ready, name: " << svc->config.name << ", pid: " << svc->pid
                    << (liveness ? ", running" : ", socket: " + svc->instanceSocketPath);
            persistInstance(svc);
            return true;
        }

        log_error << "Service " << svc->config.name
                << (liveness ? " exited during startup, pid " : " did not become ready, killing pid ") << svc->pid;
        // Guarded because a liveness failure means the child is already gone and its pid has been
        // cleared: kill(-1, SIGKILL) is not "kill nothing", it is "kill everything this user can
        // signal", which on an installation running as its own user is every euclid process.
        if (svc->pid > 0) {
            kill(svc->pid, SIGKILL);
            waitpid(svc->pid, nullptr, 0);
        }
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
        stopWatchdog();
    }

    void ServiceController::stopWatchdog() {
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

    bool ServiceController::deregisterModule(const std::string &name) {
        std::lock_guard lock(_mutex);
        return _services.erase(name) > 0;
    }

    bool ServiceController::hasService(const std::string &name) const {
        std::lock_guard lock(_mutex);
        return _services.contains(name);
    }

    void ServiceController::reconcileTransferServers() {

        // Every transfer server in the installation: this host runs them all, whatever account or
        // namespace defines them, and a reconciler that saw only one namespace's would treat every
        // other namespace's running server as undefined and tear it down.
        const auto servers = Database::RepositoryFactory::instance().etsRepository()->listAllServers("");

        // Transfer servers are spawned from the same two executables every other deployment
        // uses; only the --transfer-server argument tells one instance apart from another.
        //
        // Configured under ETS rather than as modules of their own: a transfer server is not a
        // module, it is something ETS runs, defined in the database and reconciled from there.
        // Given its own euclid.modules entry it would look to a reader like a module that has
        // been switched off, and to the module loader like one that is missing its socketPath.
        const auto &configuration = Core::Configuration::instance();
        const auto ftpExecutable = configuration.getOr<std::string>("euclid.modules.ets.ftp-executable", "/usr/local/euclid/bin/euclid-ftp");
        const auto sftpExecutable = configuration.getOr<std::string>("euclid.modules.ets.sftp-executable", "/usr/local/euclid/bin/euclid-sftp");
        const auto socketDir = configuration.getOr<std::string>("euclid.modules.ets.socket-dir", "/var/run/euclid");

        std::set<std::string> defined;

        for (const auto &server: servers) {

            // What the manager knows this server as. Everything below is keyed by it - the process
            // pool, the module row, the socket, and the argument the spawned process reads its own
            // definition back by - none of which has an account or a namespace to keep two
            // same-named servers apart. Issued once when the server was created and held ever
            // since, so none of those moves because the definition was edited.
            const auto runtimeName = Database::Entity::ETS::RuntimeName(server);
            defined.insert(runtimeName);

            const bool wantRunning = server.desiredState == Database::Entity::ETS::TransferServerState::RUNNING;
            bool registered;
            {
                std::lock_guard lock(_mutex);
                registered = _services.contains(runtimeName);
            }

            if (wantRunning && !registered) {
                Dto::ModuleConfig config;
                config.name = runtimeName;
                config.executable = server.protocol == Database::Entity::ETS::TransferProtocol::FTP ? ftpExecutable : sftpExecutable;
                config.args = {"--config", configuration.filePath().string(), "--transfer-server", runtimeName};
                config.socketPath = socketDir + "/euclid-transfer-" + runtimeName + ".sock";
                config.maxRestarts = -1;
                config.autoRestart = true;
                // Deliberately single-instance: two processes cannot share a listening port, so
                // a transfer server is not something the autoscaler can scale out.
                config.minInstances = 1;
                config.maxInstances = 1;

                log_info << "Transfer server starting, serverId: " << runtimeName
                        << ", protocol: " << Database::Entity::ETS::TransferProtocolToString(server.protocol) << ", port: " << server.port;
                registerModule(config);

                // Before start(), so a definition edited between the two is noticed on the next
                // tick rather than mistaken for what this process read.
                {
                    std::lock_guard lock(_mutex);
                    if (auto *group = getGroup(runtimeName)) group->appliedDefinition = server.runtimeFingerprint();
                }
                start(runtimeName);

            } else if (!wantRunning && registered) {
                log_info << "Transfer server stopping, serverId: " << runtimeName;
                stop(runtimeName);
                deregisterModule(runtimeName);

            } else if (wantRunning && registered) {

                // Running, and its definition may have been edited underneath it. The process
                // cannot be told - it read the definition once, as it came up - so the only way
                // to apply the change is to start it again, exactly as a changed worker count is
                // applied. Nothing else in the manager watches ETS, so an edit that is never
                // reconciled here simply never takes effect.
                const auto fingerprint = server.runtimeFingerprint();

                std::lock_guard lock(_mutex);
                auto *group = getGroup(runtimeName);
                if (group == nullptr || group->appliedDefinition == fingerprint) continue;

                // Said at warning level because it is not free: restarting drops whatever
                // transfers are in flight, and an operator who edits a busy server should find
                // out from the log why their clients were disconnected.
                log_warning << "Transfer server definition changed, restarting, serverId: " << runtimeName
                            << " - transfers in flight will be interrupted";

                group->appliedDefinition = fingerprint;
                queueRoll(*group);
            }
        }

        // Whatever this controller still runs as a transfer server but ETS no longer defines was
        // deleted while it was up, and has to be torn down. Only pools this function created are
        // considered, so a config-declared module is never touched by name collision.
        std::vector<std::string> orphaned;
        {
            std::lock_guard lock(_mutex);
            for (const auto &[name, group]: _services) {
                if (defined.contains(name)) continue;
                if (std::ranges::find(group.config.args, "--transfer-server") == group.config.args.end()) continue;
                orphaned.push_back(name);
            }
        }
        for (const auto &name: orphaned) {
            log_info << "Transfer server removed, serverId: " << name;
            stop(name);
            deregisterModule(name);
        }
    }

    namespace {

        // Where an application's artifact is materialised, one directory per application.
        //
        // By the name it runs under (Entity::EAP::RuntimeName()), not the one it is defined under:
        // a data directory is a path on a host, with no account or namespace to live in, so two
        // namespaces each defining a "billing" would otherwise be handed the same directory.
        std::filesystem::path applicationDir(const std::string &runtimeName) {
#ifdef _WIN32
            constexpr auto kDefaultDataDir = R"(C:\Program Files\euclid\data\application)";
#else
            constexpr auto kDefaultDataDir = "/usr/local/euclid/data/application";
#endif
            const auto dataDir = Core::Configuration::instance().getOr<std::string>("euclid.modules.eap.data-dir", kDefaultDataDir);
            return std::filesystem::path(dataDir) / runtimeName;
        }

        // Takes the directory away with the application, which nothing used to do: deleting an
        // application removed its row, its principal and its process, and left the artifact and
        // working files behind for good. They accumulate one directory per application ever
        // deleted, and - worse than the waste - a new application of the same name would start up
        // on top of a dead one's files.
        //
        // That second effect is the whole reason runtime names carry a random suffix (see
        // Entity::GenerateRuntimeName): a name that is never reused cannot collide with litter.
        // Cleaning up here is what makes the suffix unnecessary rather than load-bearing.
        //
        // Best effort. A directory that will not go is worth a line in the log and nothing more -
        // the application is already stopped and deregistered by this point, and throwing here
        // would abandon the rest of the reconciliation pass over a file.
        void removeApplicationDir(const std::string &runtimeName) {

            // A pool name reaches this from a database row, so it is checked rather than trusted:
            // "..", a path separator or an empty name would each resolve somewhere other than one
            // directory below the data dir, and this call deletes recursively.
            if (!Database::Entity::IsSafeRuntimeName(runtimeName)) {
                log_warning << "Refusing to remove an application directory for a suspect runtime name: " << runtimeName;
                return;
            }

            const auto directory = applicationDir(runtimeName);
            std::error_code ec;
            if (!std::filesystem::exists(directory, ec)) return;

            const auto removed = std::filesystem::remove_all(directory, ec);
            if (ec) {
                log_warning << "Could not remove application directory, path: " << directory.string() << ", error: " << ec.message();
                return;
            }
            log_info << "Removed application directory, path: " << directory.string() << ", entries: " << removed;
        }

        // Writes the application's current credentials: a bearer token for the identity it runs
        // as, and when it stops being valid.
        //
        // A file rather than an environment variable, because an environment cannot be rewritten
        // after exec() and these are meant to be replaced while the process runs - the same
        // arrangement AWS uses for container and web-identity credentials, and for the same
        // reason: a long-lived secret sitting in a process is the thing worth getting rid of.
        // The token is minted here rather than fetched from EAM: it is the same HMAC over the
        // same secret that a login would produce, and the manager already holds both.
        /**
         * @brief Reports an application whose principal cannot be found, and reports it once.
         *
         * @par
         * Once, because the reconciler runs every few seconds and a mismatch lasts until somebody
         * fixes it - a line per tick would bury the log it is trying to be found in. And once again
         * when it comes back, because "it is working now" is the other half of the story and is
         * exactly what somebody who has just changed the definition is waiting to see.
         */
        // Applies the level an application asks for its own output to be logged at - see
        // Application::logLevel. Done here, on every reconcile, rather than when the application
        // is started: the whole point of keeping the level in the row is that it can be changed
        // while the application runs, and nothing about the running processes has to change for it
        // to take effect. An application that names no level goes back under whatever
        // euclid.logging.channels says.
        //
        // Compared against what is already in force so that a level that has not changed is not
        // set - and logged - once per application per tick.
        void applyApplicationLogLevel(const Database::Entity::EAP::Application &application,
                                      const std::map<std::string, std::string> &channelLevels) {

            const auto channel = std::string(Core::LogStream::kApplicationChannel) + "."
                                 + Database::Entity::EAP::RuntimeName(application);
            const auto current = channelLevels.find(channel);

            if (application.logLevel.empty()) {
                if (current != channelLevels.end()) Core::LogStream::ClearChannelSeverity(channel);
                return;
            }
            if (current != channelLevels.end() && current->second == application.logLevel) return;

            Core::LogStream::SetChannelSeverity(channel, application.logLevel);
        }

    }// namespace

    void ServiceController::reconcileApplications() {

        // Every application in the installation: this host runs them all, whatever account or
        // namespace defines them, and a reconciler that saw only one namespace's would treat every
        // other namespace's running pool as undefined and tear it down.
        const auto applications = Database::RepositoryFactory::instance().eapRepository()->listAllApplications("");

        std::set<std::string> defined;

        // Nodes per account, filled as accounts are met, and how many applications had nowhere to
        // go - see the two uses below.
        std::map<std::string, std::vector<Database::Entity::EAP::Node> > nodesByAccount;
        long waitingForANode = 0;

        // Read once for the whole pass rather than per application: it is a copy of the level
        // table, and nothing in this loop changes it except applyApplicationLogLevel() itself.
        const auto channelLevels = Core::LogStream::ChannelSeverities();

        for (const auto &application: applications) {

            // What the manager knows this application as. Everything below is keyed by it - the
            // process pool, the module row, the directory, the socket and the log channel - none
            // of which has an account or a namespace to keep two same-named applications apart.
            // Issued once when the application was created and held ever since, so none of those
            // moves because something about the definition was edited.
            const auto runtimeName = Database::Entity::EAP::RuntimeName(application);
            defined.insert(runtimeName);

            applyApplicationLogLevel(application, channelLevels);

            // Every application is a worker's to run. The manager places it and grants the lease;
            // nothing here starts a process. Where this used to choose between running the
            // application itself and placing it on a node - isNodeApplication() - there is one
            // path: `nodes` and `nodeLabels` are constraints on which node may take an application,
            // not a switch between two implementations of starting one. docs/worker-nodes.md §13.4.
            //
            // So a host with a worker on it runs applications the way any other worker does, and a
            // host without one is a manager that runs none. That is also the security property the
            // worker was designed for and this path never had: the manager drops no privileges when
            // it spawns, so an application it ran could read euclid.json - the signing secret and
            // the database password - which §3.2 exists to prevent.

            // Nothing is handed over from a local pool here, and nothing needs to be: the only
            // code that ever registered one is the code this replaced, and an upgrade into §13.4
            // is a restart - the applications the previous manager ran were its children and went
            // with it. An instance record it left behind is dealt with by killLeftoverInstances()
            // at startup, which skips the ones a worker owns.

            // The nodes of this application's account, read once per account per pass rather than
            // once per slot: an account with three nodes and twenty applications would otherwise
            // read the node list sixty times - the same reason reconcileNodeLeases() counts
            // instances once. Also what lets the line below be said once for the installation
            // instead of once per application.
            auto nodes = nodesByAccount.find(application.accountId);
            if (nodes == nodesByAccount.end()) {
                nodes = nodesByAccount.emplace(application.accountId,
                                               Database::RepositoryFactory::instance().eapRepository()->listNodes(application.accountId))
                                .first;
            }

            if (nodes->second.empty() && application.desiredState == Database::Entity::EAP::ApplicationState::RUNNING) {
                ++waitingForANode;
                continue;
            }

            reconcileNodeApplication(application, runtimeName, nodes->second);
        }

        // Once for the installation, not once per application: an installation with no worker is
        // not twenty misconfigured applications, it is a manager without a worker - which is a
        // legitimate way to run one, and says so in one line rather than filling every tick.
        if (waitingForANode > 0) {
            log_warning << "No worker node is registered, so no application can run: " << waitingForANode
                        << " application(s) are waiting to be placed. Install euclid-wrk on this host, or register a node.";
        }

        // Whatever this controller still runs as an application but EAP no longer defines was
        // deleted while it was up, and has to be torn down. Only pools this function created are
        // considered, so a config-declared module is never touched by name collision.
        std::vector<std::string> orphaned;
        {
            std::lock_guard lock(_mutex);
            for (const auto &[name, group]: _services) {
                if (defined.contains(name)) continue;
                if (!group.config.environment.contains("EUCLID_APPLICATION_ID")) continue;
                orphaned.push_back(name);
            }
        }
        for (const auto &name: orphaned) {
            log_info << "Application removed, applicationId: " << name;
            stop(name);
            deregisterModule(name);

            // With its channel, so that an application created again under the same name does not
            // inherit a level nobody can see any more - the row that carried it is gone.
            Core::LogStream::ClearChannelSeverity(std::string(Core::LogStream::kApplicationChannel) + "." + name);
        }

        // Separately from the pools above, because a pool is not what says an application is gone.
        // One that was stopped before it was deleted has already been deregistered, so it never
        // appears as an orphan here - and stop, check, then delete is how anybody removes an
        // application. These are the names this controller has started at any point, whether or
        // not they are still registered.
        std::vector<std::string> departed;
        {
            std::lock_guard lock(_mutex);
            departed = DepartedApplicationPools(_applicationPools, defined);
            for (const auto &name: departed) _applicationPools.erase(name);
        }
        for (const auto &name: departed) removeApplicationDir(name);
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

    std::vector<std::string> ServiceController::startOrder() const {

        // Snapshot of what depends on what, taken under the lock so the sort below can work
        // without holding it.
        std::map<std::string, std::vector<std::string> > dependencies;
        {
            std::lock_guard lock(_mutex);
            for (const auto &[name, group]: _services) dependencies[name] = group.config.dependencies;
        }
        return topologicalStartOrder(dependencies);
    }


    void ServiceController::reconcileModuleSettings() {

        // Read outside the lock: this is a database call, and holding _mutex across one would
        // stall acquireInstance() for every request the gateway is routing.
        const auto modules = Database::RepositoryFactory::instance().emmRepository()->findAll();

        // What each module's own output is logged at, applied before anything else and outside the
        // lock because it changes nothing about the pool: a level is not a reason to start, stop
        // or cycle a process. See Module::logLevel, and applyApplicationLogLevel() for the same
        // thing on the application side.
        {
            const auto channelLevels = Core::LogStream::ChannelSeverities();
            for (const auto &module: modules) {
                const auto channel = std::string(Core::LogStream::kModuleChannel) + "." + module.name;
                const auto current = channelLevels.find(channel);

                if (module.logLevel.empty()) {
                    if (current != channelLevels.end()) Core::LogStream::ClearChannelSeverity(channel);
                } else if (current == channelLevels.end() || current->second != module.logLevel) {
                    Core::LogStream::SetChannelSeverity(channel, module.logLevel);
                }
            }
        }

        std::vector<std::shared_ptr<Dto::ModuleProcess> > toSpawn;
        std::vector<std::shared_ptr<Dto::ModuleProcess> > toStop;
        {
            std::lock_guard lock(_mutex);

            // Stopped first, so a module being stopped is not also spawned up to a floor on the
            // same tick - and so one being started again is sized by the limits below.
            for (const auto &module: modules) {
                auto *group = getGroup(module.name);
                if (!group || group->stopped == module.desiredStopped) continue;

                group->stopped = module.desiredStopped;
                if (group->stopped) {
                    log_info << "Module stopped, module: " << module.name << ", instances: " << group->instances.size();
                    for (auto &svc: group->instances) {
                        if (svc->pid > 0) toStop.push_back(svc);
                    }
                    // The slots stay in the pool, marked STOPPED, exactly as an ordinary stop()
                    // leaves them - so start-module has something to bring back, and nothing here
                    // has to remember how big the pool used to be.
                    group->pendingRoll.clear();
                } else {
                    log_info << "Module started, module: " << module.name;
                    while (static_cast<int>(group->instances.size()) < group->config.minInstances) {
                        auto svc = std::make_shared<Dto::ModuleProcess>();
                        svc->config = group->config;
                        group->instances.push_back(svc);
                    }
                    for (auto &svc: group->instances) {
                        if (svc->pid <= 0) toSpawn.push_back(svc);
                    }
                }
            }

            for (const auto &module: modules) {
                if (module.desiredMinInstances < 0 && module.desiredMaxInstances < 0) continue;

                auto *group = getGroup(module.name);
                if (!group) continue;

                const auto wantedMin = module.desiredMinInstances >= 0 ? module.desiredMinInstances : group->config.minInstances;
                const auto wantedMax = module.desiredMaxInstances >= 0 ? module.desiredMaxInstances : group->config.maxInstances;
                if (wantedMin == group->config.minInstances && wantedMax == group->config.maxInstances) continue;

                log_info << "Instance limits changed, module: " << module.name
                         << ", minInstances: " << group->config.minInstances << " -> " << wantedMin
                         << ", maxInstances: " << group->config.maxInstances << " -> " << wantedMax;

                group->config.minInstances = wantedMin;
                group->config.maxInstances = wantedMax;

                // Raising the floor has to bring instances up by itself: evaluateScaling() only
                // spawns on saturation or to reach desiredCount, and an idle pool is neither. A
                // lowered ceiling needs nothing here - the pool shrinks as instances go idle,
                // rather than killing one mid-request to obey a new number.
                group->desiredCount = std::max(group->desiredCount, wantedMin);
                if (group->stopped) continue;// nothing runs for a stopped module, floor or no floor
                while (static_cast<int>(group->instances.size()) < wantedMin) {
                    auto svc = std::make_shared<Dto::ModuleProcess>();
                    svc->config = group->config;
                    group->instances.push_back(svc);
                    toSpawn.push_back(svc);
                }
            }
        }

        // Stopped and spawned outside the lock, like every other one here: both wait on a process,
        // and nothing else can route a request while _mutex is held.
        for (auto &svc: toStop) {
            log_info << "Stopping " << svc->config.name << " (pid " << svc->pid << "), module stopped";
            stopInstance(svc);
        }

        for (auto &svc: toSpawn) {
            spawnInstance(svc);
        }

        reconcileWorkerThreads(modules);
        reconcileRestarts(modules);
        reconcileBackgroundWork(modules);

        // What the applications say about themselves. Nothing else can: a consumer application
        // receives no gateway request, so acquireInstance() never marks it busy and every pool of
        // them looks permanently idle to evaluateScaling().
        try {
            reconcileApplicationLoad(modules);
        } catch (const std::exception &e) {
            log_error << "Application load reconcile failed, error: " << e.what();
        }

        // Both of the above only queue; this is the one that acts, so a thread change and a
        // restart asked for on the same tick cost one restart between them rather than two.
        rollQueuedInstance();
    }

    // The labels placement matches a node against: the operator's, plus the operating system and
    // architecture the worker reported, under "os" and "arch". The reported values win over labels
    // of the same name, because a label is configuration and can say anything, while the worker's
    // binary knows what it was built for - a native aarch64 build must not land on an x86_64 box
    // labelled wrong.
    static std::map<std::string, std::string> placementLabels(const Database::Entity::EAP::Node &node) {
        auto labels = node.labels;
        if (!node.os.empty()) labels["os"] = node.os;
        if (!node.arch.empty()) labels["arch"] = node.arch;
        return labels;
    }

    // Keeps the slot records of a node application matching what its definition asks for. The
    // master creates and places them; a worker starts them and reports back.
    //
    // Nothing is spawned here, and nothing local is written: no artifact is materialised, no
    // credentials file, no process. That is the whole point - those are the worker's, on its own
    // disk, and the manager doing any of them would be doing work for a host it does not own.
    void ServiceController::reconcileNodeApplication(const Database::Entity::EAP::Application &application,
                                                     const std::string &runtimeName,
                                                     const std::vector<Database::Entity::EAP::Node> &nodes) {

        const auto emm = Database::RepositoryFactory::instance().emmRepository();

        const auto leaseSeconds = std::chrono::seconds{
                std::max(1L, Core::Configuration::instance().getOr<long>("euclid.modules.eap.node-lease-seconds", 45))};
        const auto now = std::chrono::system_clock::now();

        const auto existing = emm->findByName(runtimeName);
        const auto wantRunning = application.desiredState == Database::Entity::EAP::ApplicationState::RUNNING;

        // Stopped, or undeployed: the assignment is withdrawn rather than the processes killed.
        // The manager cannot kill them - they are on another machine - and it does not need to:
        // a slot that is no longer assigned to a node disappears from that node's next renewal,
        // and the worker stops it without being told to.
        if (!wantRunning) {
            if (!existing.has_value()) return;
            for (const auto &instance: existing->instances) {
                if (instance.assignedTo.empty()) continue;
                log_info << "Withdrawing a node slot, application: " << runtimeName
                         << ", instance: " << instance.instanceId << ", node: " << instance.assignedTo;
                std::ignore = emm->assignInstance(runtimeName, instance.instanceId, {}, {});
            }
            return;
        }

        // How many to have. The floor only: growing past it is the autoscaler's business, and it
        // works off these same records - see reconcileApplicationLoad.
        const auto wanted = std::max(1L, application.minInstances);
        const auto have = existing.has_value() ? static_cast<long>(existing->instances.size()) : 0;

        if (have >= wanted) return;

        // Who is already carrying how much of this application, which is what the spread rule
        // reads. Counted before the loop so that placing two slots in one pass does not put both
        // on the same node.
        std::map<std::string, long> perNode;
        if (existing.has_value()) {
            for (const auto &instance: existing->instances) {
                if (!instance.assignedTo.empty()) ++perNode[instance.assignedTo];
            }
        }

        const Manager::Placement::Constraints constraints{.nodes = application.nodes,
                                                          .labels = application.nodeLabels};

        for (auto slot = have; slot < wanted; ++slot) {

            std::vector<Manager::Placement::Candidate> candidates;
            for (const auto &node: nodes) {
                candidates.push_back(Manager::Placement::Candidate{.name = node.name,
                                                                   .labels = placementLabels(node),
                                                                   .cpuCount = node.cpuCount,
                                                                   .loadAverage = node.loadAverage,
                                                                   .instancesOfApplication = perNode[node.name],
                                                                   .acceptsWork = node.acceptsWork(leaseSeconds, now)});
            }

            const auto chosen = Manager::Placement::Choose(candidates, constraints);
            if (!chosen.has_value()) {
                // Said once per pass rather than once per missing slot: an application constrained
                // to a node that is not up yet would otherwise fill the log at every tick with the
                // same sentence repeated as many times as the pool is short.
                log_warning << "No node can take an instance of this application, application: " << runtimeName
                            << ", have: " << have << ", wanted: " << wanted;
                return;
            }

            // The slot itself. Deliberately carries no host and no pid: where it ends up running
            // and what pid it gets are the worker's to report, and until it does, pid -1 is what
            // keeps killLeftoverInstances from taking an interest in it.
            Database::Entity::Module module;
            module.name = runtimeName;
            module.executable = Database::Entity::EAP::RuntimeCommandPrefix(application.runtime).empty()
                                        ? application.command
                                        : Database::Entity::EAP::RuntimeCommandPrefix(application.runtime).front();
            module.active = true;
            // A definition counts instances in a long and a module row in an int, so the narrowing
            // is explicit here as it is on the local path - see the registerModule() call above.
            module.minInstances = static_cast<int>(application.minInstances);
            module.maxInstances = static_cast<int>(application.maxInstances);

            Database::Entity::ModuleInstance instance;
            instance.instanceId = runtimeName + "-" + Core::UuidUtils::CreateRandomUuid();
            instance.state = Database::Entity::ModuleState::STOPPED;
            instance.assignedTo = *chosen;
            instance.leaseExpiresAt = now + leaseSeconds;

            emm->upsertInstance(module, instance);
            ++perNode[*chosen];

            log_info << "Placed an instance of a node application, application: " << runtimeName
                     << ", instance: " << instance.instanceId << ", node: " << *chosen;
        }
    }

    // Slots on a node whose lease has run out: given to another node, or left alone and said out
    // loud. The master's half of docs/worker-nodes.md §5.
    //
    // The asymmetry with the worker is the safety property, and it is worth restating where the
    // code is. The worker stops its instances the moment its own deadline passes, with no margin;
    // the master waits until that deadline *plus* a margin for clock skew before giving the work to
    // anybody else. The gap between the two is a window in which nobody is running the slot - which
    // is the direction this is allowed to be wrong in. Two of something that must run once is worse
    // than none of it for one lease period.
    //
    // Nothing here kills a process. It cannot: the process is on another machine, and a pid from
    // there means nothing locally. All this does is move the claim, and the worker that still holds
    // the slot - if it is executing at all - has already let go of it by its own clock.
    void ServiceController::reconcileNodeLeases() {

        const auto eap = Database::RepositoryFactory::instance().eapRepository();
        const auto emm = Database::RepositoryFactory::instance().emmRepository();

        const auto leaseSeconds = std::chrono::seconds{
                std::max(1L, Core::Configuration::instance().getOr<long>("euclid.modules.eap.node-lease-seconds", 45))};

        // How much clock skew between a node and this host is tolerated before its lease counts as
        // past. Generous on purpose: the cost of waiting too long is an outage of one slot, and the
        // cost of not waiting long enough is two processes doing one job.
        const auto margin = std::chrono::seconds{
                std::max(0L, Core::Configuration::instance().getOr<long>("euclid.modules.eap.node-lease-skew-seconds", 10))};

        const auto modules = emm->findAll();

        // Nodes, with how many instances of each application they already run - which is what
        // placement's spread rule needs. Counted across the whole installation once rather than per
        // slot, because an account with three nodes and twenty applications would otherwise read
        // every module document twenty times.
        std::map<std::string, std::map<std::string, long> > instancesPerNodePerModule;
        for (const auto &module: modules) {
            for (const auto &instance: module.instances) {
                if (!instance.assignedTo.empty()) ++instancesPerNodePerModule[instance.assignedTo][module.name];
            }
        }

        const auto now = std::chrono::system_clock::now();

        for (const auto &module: modules) {
            for (const auto &instance: module.instances) {

                // Only slots given to a node. An instance the manager runs itself holds no lease,
                // and leaseHasExpired() answers false for it - but checking the assignment first
                // says why rather than relying on that.
                if (instance.assignedTo.empty()) continue;
                if (!instance.leaseHasExpired(margin, now)) continue;

                const auto application = eap->findApplicationByRuntimeName(module.name);
                if (!application.has_value()) {
                    // The pool outlived its definition. Nothing to place, and the application
                    // reconcile will take the pool down - so this says nothing and waits.
                    continue;
                }

                std::vector<Manager::Placement::Candidate> candidates;
                for (const auto &node: eap->listNodes(application->accountId)) {
                    candidates.push_back(Manager::Placement::Candidate{
                            .name = node.name,
                            .labels = placementLabels(node),
                            .cpuCount = node.cpuCount,
                            .loadAverage = node.loadAverage,
                            .instancesOfApplication = instancesPerNodePerModule[node.name][module.name],
                            // The node whose lease lapsed is excluded by this and needs no special
                            // case: a lease only expires because the node stopped renewing, and a
                            // node that stopped renewing is not live. If it comes back it will be
                            // live again - and by then it has already stopped the slot itself.
                            .acceptsWork = node.acceptsWork(leaseSeconds, now)});
                }

                const Manager::Placement::Constraints constraints{.nodes = application->nodes,
                                                                  .labels = application->nodeLabels};

                const auto chosen = Manager::Placement::Choose(candidates, constraints);
                if (!chosen.has_value()) {
                    // No eligible node. The slot stays where it is rather than being forced onto a
                    // machine that cannot run it, or silently taken over by this host - which for
                    // an application that named a node or a label would be running it somewhere it
                    // said it must not.
                    log_warning << "Lease expired and no node can take the slot, module: " << module.name
                                << ", instance: " << instance.instanceId << ", was on: " << instance.assignedTo;
                    continue;
                }
                if (*chosen == instance.assignedTo) {
                    // The same node, which means it is live again and simply had not renewed in
                    // time. Extending rather than moving: it is the node already holding the slot,
                    // and it has stopped the process by its own clock, so it will start it again on
                    // its next tick.
                    log_info << "Lease expired but the node is back, extending, module: " << module.name
                             << ", instance: " << instance.instanceId << ", node: " << *chosen;
                } else {
                    log_warning << "Lease expired, re-placing slot, module: " << module.name
                                << ", instance: " << instance.instanceId
                                << ", from: " << instance.assignedTo << ", to: " << *chosen;
                }

                if (emm->assignInstance(module.name, instance.instanceId, *chosen, now + leaseSeconds)) {
                    ++instancesPerNodePerModule[*chosen][module.name];
                }
            }
        }
    }

    void ServiceController::reconcileApplicationLoad(const std::vector<Database::Entity::Module> &modules) {

        // An application receives no gateway request, so acquireInstance() never marks it busy and
        // there is nothing to scale on but what the application says about itself. It can say it
        // two ways, and this prefers the quick one.
        //
        // @par The direct road
        // `eap report-load` writes onto the instance's own record - these very module documents -
        // so it costs no query and is as fresh as the application's reporting interval. About
        // twenty seconds from a load change to the pool changing.
        //
        // @par The old road, still here
        // Pushing `application-utilisation` to EMO as a metric. EMO accumulates samples in memory
        // and writes a row only when its averaging bucket closes - euclid.modules.emo.average-period,
        // five minutes as shipped - so the figure arrives up to five minutes late. That is what
        // made scaling slow, and it is why the direct road exists.
        //
        // @par Why both
        // The SDKs are released separately from euclid, so an application built against a published
        // euclid-jdk cannot report the new way however new the installation is. Reading only the
        // new road means those applications report nothing the autoscaler can see and their pools
        // never grow at all - which is worse than slow. So the fallback is per pool: a pool with no
        // instance reporting directly is read from EMO exactly as it always was, and one that does
        // report never pays for the query.
        const auto now = std::chrono::steady_clock::now();
        const auto fresh = std::chrono::system_clock::now() - std::chrono::seconds(LoadFreshnessSeconds());

        std::vector<std::string> needFallback;
        {
            std::lock_guard lock(_mutex);
            for (const auto &module: modules) {

                auto *group = getGroup(module.name);
                if (!group) continue;

                bool reporting = false;
                long pending = 0;
                long reportingInstances = 0;

                for (const auto &reported: module.instances) {

                    auto found = std::ranges::find_if(group->instances, [&](const auto &svc) {
                        return svc->instanceId == reported.instanceId;
                    });
                    if (found == group->instances.end()) continue;
                    auto &svc = *found;
                    if (svc->state != Database::Entity::ModuleState::RUNNING) continue;

                    // Never reported, or stopped reporting.
                    if (reported.utilisation < 0 || reported.loadReportedAt < fresh) continue;
                    reporting = true;

                    svc->utilisation = reported.utilisation;
                    svc->loadReportedAt = reported.loadReportedAt;

                    // Two questions of one figure, and they need different bars - see
                    // UtilisationSignals.h. Working keeps the idle timer from running; only
                    // saturated asks for another instance.
                    if (IsWorking(reported.utilisation)) group->lastActivityAt = now;
                    if (IsSaturated(reported.utilisation)) svc->wasBusySinceLastCheck = true;
                    ++reportingInstances;
                    if (reported.backlog > 0) pending += reported.backlog;
                }

                if (!reporting) {
                    // Nothing on the direct road. Either the application does not report at all, in
                    // which case EMO has nothing either and the fallback finds nothing, or it is an
                    // older build still pushing metrics - which is the case worth catching.
                    needFallback.push_back(module.name);
                    continue;
                }

                applyUnreportedAreBusy(*group, fresh, now);
                applyBacklog(*group, pending, reportingInstances, now);
            }
        }

        if (!needFallback.empty()) reconcileApplicationLoadFromMetrics(needFallback);
    }

    // The pre-2026-09-14 path, kept for applications built against a published SDK that does not
    // know `eap report-load` yet. Slow by construction - see reconcileApplicationLoad() - and only
    // read for the pools that gave nothing better.
    void ServiceController::reconcileApplicationLoadFromMetrics(const std::vector<std::string> &poolNames) {

        // Read once for every instance rather than once per instance: one query for each metric,
        // matched up by label below. A window rather than "the latest row" because EMO writes a
        // bucket per period - anything older than this has stopped reporting. EMO's averaging
        // period is what decides how old the newest row can be, so the window is read from the same
        // setting rather than guessed at here.
        const auto period = Core::Configuration::instance().getOr<long>("euclid.modules.emo.average-period", 300);
        const auto since = std::chrono::system_clock::now() - std::chrono::seconds(period * kLoadFreshnessPeriods);
        const auto repo = Database::RepositoryFactory::instance().emoRepository();

        auto latestByInstance = [&](const std::string &metric) {
            std::map<std::string, Database::Entity::Monitoring::MonitoringData> newest;
            Database::MonitoringQuery query;
            query.name = metric;
            query.labelName = "instance";
            query.from = since;
            query.resolution = Database::Entity::Monitoring::Resolution::RAW;
            query.limit = kLoadSampleLimit;
            // list() returns most recent first, so the first row seen for a label is its latest.
            for (const auto &row: repo->list(query)) {
                // By the dimension this asked for by name rather than by whichever label happens
                // to be first: a row pushed by an application carries as many as it likes, and
                // "instance" is the only one that identifies the pool member here.
                const auto instance = row.labels.find("instance");
                if (instance == row.labels.end()) continue;
                newest.try_emplace(instance->second, row);
            }
            return newest;
        };

        const auto utilisation = latestByInstance("application-utilisation");
        const auto backlog = latestByInstance("application-backlog");
        if (utilisation.empty() && backlog.empty()) return;

        // No freshness cutoff here, unlike the direct road. LoadFreshnessSeconds() is 45 seconds -
        // an application's own reporting interval - and nothing on this path reports directly:
        // every figure has been through EMO's buckets, which are one per averaging period, 300
        // seconds by default. Measuring a 300-second bucket against a 45-second cutoff would
        // discard every sample and the fallback would find nothing at all. What bounds staleness
        // here is the query window above, `since`, read from the same averaging period.
        const auto now = std::chrono::steady_clock::now();

        std::lock_guard lock(_mutex);
        for (const auto &name: poolNames) {

            auto *group = getGroup(name);
            if (!group) continue;

            bool reporting = false;
            long pending = 0;
            long reportingInstances = 0;

            for (auto &svc: group->instances) {
                if (svc->state != Database::Entity::ModuleState::RUNNING) continue;

                const auto sample = utilisation.find(svc->instanceId);
                if (sample == utilisation.end()) continue;
                reporting = true;

                // The peak within the bucket, not its average. A burst that saturates an instance
                // for twenty seconds and then stops averages down to almost nothing over a
                // five-minute bucket - which is exactly the load worth reacting to, reported as
                // though it never happened.
                if (IsWorking(sample->second.maxValue)) group->lastActivityAt = now;
                if (IsSaturated(sample->second.maxValue)) svc->wasBusySinceLastCheck = true;

                ++reportingInstances;
                if (const auto depth = backlog.find(svc->instanceId); depth != backlog.end()) {
                    pending += static_cast<long>(depth->second.maxValue);
                }
            }

            if (!reporting) continue;

            // Same rule as the direct road, but keyed on whether EMO has a sample for the instance
            // rather than on how fresh its own report is.
            for (auto &svc: group->instances) {
                if (svc->state != Database::Entity::ModuleState::RUNNING) continue;
                if (utilisation.contains(svc->instanceId)) continue;
                if (std::chrono::duration_cast<std::chrono::seconds>(now - svc->startTime).count() < LoadFreshnessSeconds()) continue;
                svc->wasBusySinceLastCheck = true;
            }

            applyBacklog(*group, pending, reportingInstances, now);
        }
    }

    // An instance that has stopped reporting is not an idle instance - it is an instance nothing is
    // known about, and a crashed reporter would otherwise read as 0% and be the first one stopped.
    // Unknown counts as busy, so scale-down passes it over.
    //
    // With one exception, and it is the difference between a pool that ramps and one that runs
    // away: an instance that has only just started has not had time to report, and counting it busy
    // makes every scale-up look like continued saturation and spawn another. It is given one
    // reporting window to say something before it counts either way.
    void ServiceController::applyUnreportedAreBusy(ServiceGroup &group, const std::chrono::system_clock::time_point fresh,
                                                   const std::chrono::steady_clock::time_point now) {

        for (auto &svc: group.instances) {
            if (svc->state != Database::Entity::ModuleState::RUNNING) continue;
            if (svc->loadReportedAt >= fresh) continue;
            if (std::chrono::duration_cast<std::chrono::seconds>(now - svc->startTime).count() < LoadFreshnessSeconds()) continue;
            svc->wasBusySinceLastCheck = true;
        }
    }

    // Work waiting that nobody is getting to is the one signal utilisation cannot give: an instance
    // is either busy or not, and "busy" says nothing about how much is left. Raising desiredCount is
    // how evaluateScaling() is asked for more, and it is bounded there by maxInstances.
    void ServiceController::applyBacklog(ServiceGroup &group, const long pending, const long reporting,
                                        const std::chrono::steady_clock::time_point now) {

        if (reporting <= 0) return;

        // Any work waiting at all means the pool is not idle, whatever its utilisation says, and
        // this is before the threshold below on purpose: letting the idle timer run out from under
        // a pool with a queue in front of it is what made this oscillate, stopping the instance
        // doing the work about a minute after it finished starting.
        if (pending > 0) group.lastActivityAt = now;

        // The mean, and a target rather than an increment - see InstancesForBacklog() for both,
        // which is a header of its own so the arithmetic can be tested.
        const auto wanted = InstancesForBacklog(pending, reporting, kBacklogScaleUpMessages);

        // Both ways now: up to what the backlog asks for at once, down towards it one instance per
        // tick. It used to only ever rise, on the reasoning that coming down belonged to the idle
        // branch - but that branch cannot run on a pool with a queue in front of it, so on an
        // application nothing lowered the target at all. See NextDesiredCount().
        const auto previous = group.desiredCount;
        group.desiredCount = NextDesiredCount(group.desiredCount, wanted, group.config.minInstances,
                                              group.config.maxInstances);
        if (group.desiredCount == previous) return;

        log_info << "Application backlog, module: " << group.config.name
                 << ", pending per instance: " << pending / reporting
                 << " (" << pending << " reported across " << reporting << ")"
                 << ", desiredCount: " << previous << " -> " << group.desiredCount;
    }

    void ServiceController::reconcileBackgroundWork(const std::vector<Database::Entity::Module> &modules) {

        // What a module says about itself, copied onto the pool so evaluateScaling() can read it
        // without a database call in a loop that runs every second under the lock.
        //
        // It is the one thing the manager cannot observe. An --async purge is answered at once and
        // carried on afterwards on a thread, so acquireInstance()/releaseInstance() put the
        // instance back to idle while the work runs - and scale-down stopped exactly that
        // instance, taking the removal with it.
        std::lock_guard lock(_mutex);
        for (const auto &module: modules) {
            auto *group = getGroup(module.name);
            if (!group) continue;

            for (const auto &reported: module.instances) {
                for (auto &svc: group->instances) {
                    if (svc->instanceId != reported.instanceId) continue;
                    svc->backgroundTasks = reported.backgroundTasks;
                    break;
                }
            }
        }
    }

    void ServiceController::reconcileWorkerThreads(const std::vector<Database::Entity::Module> &modules) {

        std::lock_guard lock(_mutex);
        for (const auto &module: modules) {
            auto *group = getGroup(module.name);
            if (!group) continue;

            // The first time a module is seen, whatever is stored is already what its running
            // instances started with - they read it themselves as they came up - so it is
            // recorded and nothing is restarted. Without this, every manager start would roll
            // every module that has ever had its thread count set.
            if (group->appliedThreads == ServiceGroup::kThreadsUnobserved) {
                group->appliedThreads = module.desiredThreads;
                continue;
            }
            if (module.desiredThreads == group->appliedThreads) continue;

            log_info << "Worker threads changed, module: " << module.name
                     << ", threads: " << group->appliedThreads << " -> " << module.desiredThreads
                     << ", restarting " << group->instances.size() << " instance(s)";

            // A thread count is fixed when the io_context's threads are created, so the only way
            // to apply a new one is to start the process again.
            group->appliedThreads = module.desiredThreads;
            queueRoll(*group);
        }
    }

    void ServiceController::reconcileRestarts(const std::vector<Database::Entity::Module> &modules) {

        std::lock_guard lock(_mutex);
        for (const auto &module: modules) {
            auto *group = getGroup(module.name);
            if (!group) continue;

            // Same first-observation rule as the thread count, and for a stronger reason: a
            // request made before this manager started has already been satisfied by it starting.
            // Without this, one restart-module would restart the module again after every manager
            // start, forever.
            if (!group->restartObserved) {
                group->restartObserved = true;
                group->appliedRestartAt = module.restartRequestedAt;
                continue;
            }
            if (module.restartRequestedAt <= group->appliedRestartAt) continue;

            group->appliedRestartAt = module.restartRequestedAt;
            if (group->stopped) {
                // Nothing to restart, and starting it would undo what stop-module asked for. The
                // request is still recorded as handled, so it is not carried out later on.
                log_info << "Restart requested for a stopped module, ignoring, module: " << module.name;
                continue;
            }

            log_info << "Restart requested, module: " << module.name << ", restarting " << group->instances.size() << " instance(s)";
            queueRoll(*group);
        }
    }

    void ServiceController::queueRoll(ServiceGroup &group) {
        // Replaces rather than appends: whatever the pool looks like now is what should be
        // cycled, and an instance left over from a superseded request would be restarted twice.
        group.pendingRoll.clear();
        for (auto &svc: group.instances) {
            if (svc->pid > 0) group.pendingRoll.push_back(svc);
        }
    }

    void ServiceController::rollQueuedInstance() {

        std::shared_ptr<Dto::ModuleProcess> toRoll;
        int restartDelayMs = 1000;
        {
            std::lock_guard lock(_mutex);

            // One instance per tick, across all modules: this runs on the watchdog thread, and
            // stopping and starting a process is not quick. A pool of any size keeps serving from
            // its other instances while it is worked through.
            for (auto &group: _services | std::views::values) {
                while (!group.pendingRoll.empty() && !toRoll) {
                    auto candidate = group.pendingRoll.back();
                    group.pendingRoll.pop_back();
                    // Skip anything that has since been scaled down or given up on: the slot is
                    // gone, and starting it again would put back an instance nobody wants.
                    if (!std::ranges::contains(group.instances, candidate)) continue;
                    toRoll = std::move(candidate);
                    restartDelayMs = group.config.restartDelayMs;
                }
                if (toRoll) break;
            }
        }

        if (!toRoll) return;

        log_info << "Restarting " << toRoll->config.name << " instance, pid: " << toRoll->pid;
        stopInstance(toRoll);
        std::this_thread::sleep_for(std::chrono::milliseconds(restartDelayMs));
        spawnInstance(toRoll);
    }

    bool ServiceController::modulesRunning() const {
        std::lock_guard lock(_mutex);
        for (const auto &group: _services | std::views::values) {
            if (!group.config.core) continue;
            // A module nobody wants running is not one to wait for - otherwise stopping any one
            // of them would keep every application from ever starting.
            if (group.stopped) continue;
            if (std::ranges::none_of(group.instances, [](const auto &svc) {
                return svc->state == Database::Entity::ModuleState::RUNNING;
            })) {
                return false;
            }
        }
        return true;
    }

    bool ServiceController::dependenciesRunning(const Dto::ModuleConfig &config) const {
        if (config.dependencies.empty()) return true;

        std::lock_guard lock(_mutex);
        for (const auto &dependency: config.dependencies) {
            const auto group = _services.find(dependency);
            // Not registered at all: inactive or misspelled. Treated as satisfied rather than
            // blocking forever - startOrder() has already said so in the log, and a typo should
            // not be able to stop an application from ever starting.
            if (group == _services.end()) continue;

            if (std::ranges::none_of(group->second.instances, [](const auto &svc) {
                return svc->state == Database::Entity::ModuleState::RUNNING;
            })) {
                return false;
            }
        }
        return true;
    }

    void ServiceController::startDocumentStore() {

        // The module that holds the store, on the one backend where the store is a module at all.
        constexpr auto kStoreModule = "emd";

        const auto &configuration = Core::Configuration::instance();
        if (configuration.getOr<std::string>("euclid.database.backend", "mongodb") != kStoreModule) return;

        int timeoutMs = 0;
        {
            std::lock_guard lock(_mutex);
            const auto *group = getGroup(kStoreModule);
            if (!group) {
                // Configured to read from a module that is not configured. Nothing here can fix
                // that, and saying so once is better than the alternative - every query this
                // manager makes failing against a socket that will never exist.
                log_error << "Database backend is '" << kStoreModule << "' but no such module is configured; "
                          << "the manager has no store to read from";
                return;
            }
            // The module's own readiness budget, which is what every other module is given to
            // produce a socket - there is no reason for the store to be held to a different one.
            timeoutMs = group->config.readyTimeoutMs;
        }

        // Out of dependency order on purpose: every other module waits for what it depends on, and
        // this is the one the manager itself depends on. start() only spawns instances that are
        // not already running, so the ordinary pass over the start order below finds it up and
        // spawns nothing.
        start(kStoreModule);

        // The address the store client connects to, which is the one emd binds for itself - not
        // the per-instance socket the manager handed it. Read from the same configuration key
        // RepositoryFactory uses, so the two cannot drift apart.
        const auto socketPath = configuration.getOr<std::string>(
                "euclid.modules." + std::string(kStoreModule) + ".socketPath", "/var/run/euclid/euclid-emd.sock");

        if (!waitForSocket(socketPath, timeoutMs)) {
            // Not a reason to refuse to start: the store may yet come up, and every repository call
            // retries its own connect anyway. What is lost is the read below, so this says what
            // that costs rather than only that a timeout happened.
            log_error << "The memory database is not listening at " << socketPath << " after " << timeoutMs
                      << "ms; starting every module as though none had been stopped";
            return;
        }

        log_info << "Memory database ready, socket: " << socketPath;
    }

    void ServiceController::startAll() {

        // Before the read below rather than after: on the emd backend that read goes to a module
        // this manager starts, and asking before it is listening is why a fresh start logged an
        // error for every query it made on its way up.
        startDocumentStore();

        // What was stopped through "emm stop-module" is desired state, so it outlives the manager:
        // read before anything is started, rather than started and then stopped again seconds
        // later by the first reconcile tick.
        try {
            std::lock_guard lock(_mutex);
            for (const auto &module: Database::RepositoryFactory::instance().emmRepository()->findAll()) {
                if (auto *group = getGroup(module.name)) group->stopped = module.desiredStopped;
            }
        } catch (const std::exception &e) {
            // Not fatal: a manager that cannot reach the database has bigger problems than a
            // module that comes up when it should not have, and the reconcile will stop it anyway.
            log_error << "Could not read which modules are stopped, starting all of them, error: " << e.what();
        }

        const auto names = startOrder();

        std::string order;
        for (const auto &name: names) {
            if (!order.empty()) order += " -> ";
            order += name;
        }
        log_info << "Starting modules in dependency order: " << order;

        // start() blocks until the instance is ready (or gives up on it), so by the time this
        // returns for one module, everything it depends on is already answering - which is the
        // whole point of the ordering. What it does not guarantee is that a dependency has
        // finished whatever it does lazily on its first request; a module that must not be called
        // before it is truly warm should do that work before it creates its socket.
        for (const auto &name: names) {
            bool stopped = false;
            {
                std::lock_guard lock(_mutex);
                if (const auto *group = getGroup(name)) stopped = group->stopped;
            }
            if (stopped) {
                log_info << "Not starting module, module: " << name << ", stopped through emm stop-module";
                continue;
            }
            start(name);
        }
    }

    std::vector<std::string> ServiceController::stopOrder() const {

        // Which pools are what. A transfer server and an application are both non-core pools to
        // this controller, and only their own modules' definitions say which is which - so they
        // are asked, rather than the distinction being guessed from a name.
        std::set<std::string> transferServers;
        std::set<std::string> applications;
        try {
            for (const auto &server: Database::RepositoryFactory::instance().etsRepository()->listAllServers("")) {
                // Matched against the pool names in _services below, which are runtime names.
                transferServers.insert(Database::Entity::ETS::RuntimeName(server));
            }
            for (const auto &application: Database::RepositoryFactory::instance().eapRepository()->listAllApplications("")) {
                // Matched against the pool names in _services below, which are runtime names.
                applications.insert(Database::Entity::EAP::RuntimeName(application));
            }
        } catch (const std::exception &e) {
            // Shutdown must not depend on the database being reachable. Without the definitions
            // everything falls into the last tier, which is the order this had before.
            log_warning << "Could not read the transfer server and application lists for shutdown ordering, error: " << e.what();
        }

        std::map<std::string, std::vector<std::string> > dependencies;
        std::vector<std::string> gateway, ingress, apps;
        {
            std::lock_guard lock(_mutex);
            for (const auto &[name, group]: _services) {
                if (name == kApiGateway) gateway.push_back(name);
                else if (transferServers.contains(name)) ingress.push_back(name);
                else if (applications.contains(name)) apps.push_back(name);
                else dependencies[name] = group.config.dependencies;
            }
        }

        // Stopped in the order that leaves the fewest requests with nowhere to go.
        //
        // The API gateway goes first. It is a core module, but it is also where external HTTP
        // traffic enters, and everything it proxies to is stopped below it - left for the module
        // tier it would spend the whole shutdown handing requests to applications that are
        // already gone, and answering the outside world with 502s it could simply have refused.
        //
        // Then the transfer servers: they are where work enters the installation, and stopping
        // them means nothing new arrives while everything else is being taken down. Then the
        // applications, which are the only things that consume on their own initiative - an
        // application outliving the queue it polls spends its last seconds failing, which is what
        // an alphabetical shutdown produced.
        //
        // The modules go last and in reverse start order, so a module is stopped before whatever
        // it depends on: the same dependency graph that decides the order they come up in, read
        // backwards.
        auto order = gateway;
        order.insert(order.end(), ingress.begin(), ingress.end());
        order.insert(order.end(), apps.begin(), apps.end());

        auto modules = topologicalStartOrder(dependencies);
        std::ranges::reverse(modules);
        order.insert(order.end(), modules.begin(), modules.end());
        return order;
    }

    void ServiceController::stopClients() {
        const auto names = stopOrder();

        // Only the pools that talk *to* euclid rather than being talked to: the transfer servers
        // and the applications, which stopOrder() puts first for exactly this reason - plus the
        // API gateway, which is core but belongs to this tier all the same, since it is a client
        // of the applications and has to be out of the way before they go.
        std::set<std::string> modules;
        {
            std::lock_guard lock(_mutex);
            for (const auto &[name, group]: _services) {
                if (group.config.core && name != kApiGateway) modules.insert(name);
            }
        }

        std::vector<std::string> clients;
        for (const auto &name: names) {
            if (!modules.contains(name)) clients.push_back(name);
        }
        if (clients.empty()) return;

        std::string order;
        for (const auto &name: clients) {
            if (!order.empty()) order += " -> ";
            order += name;
        }
        log_info << "Stopping clients before the gateway: " << order;

        for (const auto &name: clients) stop(name);
    }

    void ServiceController::stopAll() {
        const auto names = stopOrder();

        std::string order;
        for (const auto &name: names) {
            if (!order.empty()) order += " -> ";
            order += name;
        }
        log_info << "Stopping modules in shutdown order: " << order;

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

    std::optional<InstanceHandle> ServiceController::acquireInstance(const std::string &name, const bool countsAsLoad) {
        std::lock_guard lock(_mutex);
        auto *group = getGroup(name);
        if (!group || group->instances.empty()) return std::nullopt;

        const size_t n = group->instances.size();
        for (size_t i = 0; i < n; ++i) {
            const size_t idx = (group->rrCursor + i) % n;
            if (const auto &svc = group->instances[idx]; svc->state == Database::Entity::ModuleState::RUNNING) {
                group->rrCursor = (idx + 1) % n;

                // A request that is deliberately waiting is not a busy instance. receive-messages
                // and receive-events hold their connection for the whole wait the caller asked
                // for - twenty seconds by default - and for almost all of it the module is doing
                // nothing at all, because there is nothing to do. Counting that as load inverts
                // the signal completely: an installation with no traffic and a dozen idle
                // consumers looks permanently saturated, so its pools scale up and can never
                // scale down, which is exactly what happens without this.
                //
                // The work such a request does when a message *is* there is real, but it is brief
                // and it is what the producer side already makes visible - send-message marks the
                // pool busy on its way in.
                svc->inFlightRequests++;
                if (countsAsLoad) {
                    svc->activeRequests++;
                    svc->wasBusySinceLastCheck = true;
                    group->lastActivityAt = std::chrono::steady_clock::now();
                }
                return InstanceHandle{.socketPath = svc->instanceSocketPath, .pid = svc->pid};
            }
        }
        return std::nullopt;
    }

    void ServiceController::releaseInstance(const std::string &name, const pid_t pid, const bool countsAsLoad) {
        std::lock_guard lock(_mutex);
        const auto *group = getGroup(name);
        if (!group) return;
        for (auto &svc: group->instances) {
            if (svc->pid != pid) continue;
            if (svc->inFlightRequests > 0) svc->inFlightRequests--;
            // Only what was counted on the way in is given back; decrementing regardless would
            // take the load count below what the instance is actually serving.
            if (!countsAsLoad) return;
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
        _scaleUpSaturatedTicks = std::max(1L, Core::Configuration::instance().getOr<long>("euclid.scaling.scale-up-saturated-ticks", 3));
        _usesMongoBackend = Core::Configuration::instance().getOr<std::string>("euclid.database.backend", "mongodb") != "memory";

        _watchdog = std::thread([this] {
            while (_running) {
                std::this_thread::sleep_for(std::chrono::seconds(1));

                // On POSIX this duplicates the SIGCHLD-driven call in main.cpp's dispatch
                // loop (harmless - waitpid(WNOHANG) just returns immediately when there's
                // nothing to reap). On Windows it's the only way instances get reaped at
                // all, since there's no SIGCHLD/waitpid(-1) equivalent there.
                onChildExit();

                // Transfer server definitions live in the database, so reconciling is pointless
                // while it is unreachable - and would read an empty list as "delete everything".
                // Every few ticks rather than every one: spawning a process is not urgent, and
                // this reads the whole definition set each time.
                if (_databaseWasReachable && ++_transferReconcileTick >= kTransferReconcileTicks) {
                    _transferReconcileTick = 0;
                    try {
                        reconcileTransferServers();
                    } catch (const std::exception &e) {
                        log_error << "Transfer server reconcile failed, error: " << e.what();
                    }
                    // Applications ride the same tick, for the same reasons - and separately, so
                    // that one reconcile throwing does not stop the other from running.
                    try {
                        reconcileApplications();
                    } catch (const std::exception &e) {
                        log_error << "Application reconcile failed, error: " << e.what();
                    }

                    // And the instance limits and thread counts somebody asked for through
                    // "emm set-instances" / "emm set-threads", which are only a record in the
                    // database until this applies them.
                    try {
                        reconcileModuleSettings();
                    } catch (const std::exception &e) {
                        log_error << "Module settings reconcile failed, error: " << e.what();
                    }

                    // And slots on a worker whose lease has run out, which is the master's half of
                    // the safety argument in docs/worker-nodes.md §5.
                    try {
                        reconcileNodeLeases();
                    } catch (const std::exception &e) {
                        log_error << "Node lease reconcile failed, error: " << e.what();
                    }
                }

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
                        // Same reason as in evaluateScaling(): a module somebody stopped should
                        // stay stopped, including when one of its instances was mid-backoff at
                        // the moment it was asked for.
                        if (group.stopped) continue;

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
                    releaseHttpPort(svc->instanceId);
                    Database::RepositoryFactory::instance().emmRepository()->removeInstance(svc->config.name, svc->instanceId);
                }
            }
        });
    }

    void ServiceController::handleExitedInstance(const std::shared_ptr<Dto::ModuleProcess> &svc, const int status) {
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
        log_warning << "Service " << svc->config.name << " crashed (pid=" << exitedPid << ")"
                    << describeExit(status, svc->config.executable, svc->config.workingDir);

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
            closeInstanceHandles(svc);
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
            handleExitedInstance(svc, status);
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
        // PENDING_RESTART, not CRASHED: the watchdog's restart pass - backoff, restart budget,
        // "was stable, resetting backoff" - only ever looks at instances in that state. Leaving a
        // crashed instance CRASHED told the database the truth and left the module down for good,
        // so a module that died once stayed unreachable until euclid itself was restarted, with
        // the gateway answering "service 'x' not registered or not running" in the meantime.
        //
        // The crash is still recorded: handleExitedInstance() persists CRASHED before calling
        // this, and the next state written is the restart's.
        svc->state = Database::Entity::ModuleState::PENDING_RESTART;
    }

    bool ServiceController::stopInstance(const std::shared_ptr<Dto::ModuleProcess> &svc, const int timeoutMs) {
        if (!svc || svc->pid <= 0) return false;

        svc->state = Database::Entity::ModuleState::STOPPING;
        log_info << "Stopping instance '" << svc->config.name << "' (pid " << svc->pid << ")";

#if defined(_WIN32)
        // Said once per instance rather than left to look like a slow shutdown: an instance with
        // no stop event is never asked to stop at all, so it always burns the whole timeout below
        // and is then killed - and the log would otherwise read exactly like a module that was
        // asked politely and ignored it.
        if (!Platform::RequestGracefulStop(svc->stopEvent)) {
            log_warning << "Instance '" << svc->config.name << "' (pid " << svc->pid
                        << ") has no stop event and cannot be asked to shut down, it will be terminated";
        }
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
        closeInstanceHandles(svc);
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
            // A stopped module is not idle, it is off. Scaling it either way would undo what
            // stop-module asked for, one tick after it was asked.
            if (group.stopped) continue;

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

            // Scale up only once the pool is actually saturated (every running instance was busy
            // at some point in the last tick), not just because SOME instance was touched at all.
            // wasBusySinceLastCheck already solves the sub-tick sampling problem (a busy window
            // shorter than the 1s poll interval still counts, see its doc comment), so by the time
            // we get here "busy" is already an accurate picture of the last ~1s, not a
            // point-in-time snapshot - requiring busy == running on top of that is a genuine
            // saturation signal, not the "requires impossible exact-instant alignment" problem an
            // earlier version of this check was written to work around. Scaling up on ANY busy
            // instance (the previous condition) meant a single instance handling purely sequential,
            // non-concurrent traffic - e.g. one euclid-cli command after another - looked identical
            // to genuine concurrent overload: every request momentarily marks its instance busy,
            // and since round-robin doesn't prefer idle instances, each one's next watchdog tick
            // saw "an instance was busy" and spawned another, walking the pool all the way to
            // maxInstances even though only one caller was ever active at a time.
            // running < desiredCount fires scale-up proactively, ahead of any busy sample, when a
            // client has declared it's about to need more instances than the pool currently has -
            // see declareExpectedConcurrency()'s doc comment for why busy-based detection alone
            // can't be trusted to catch this for high-instance-count/short-request workloads.
            const bool saturated = running > 0 && busy >= running;

            // Held across ticks rather than decided on this one: see saturatedTicks. The count is
            // what separates a pool that is actually out of capacity from one that was asked a
            // single question - at running == 1 the test above cannot tell those apart, because one
            // request makes every running instance busy.
            if (saturated) {
                ++group.saturatedTicks;
            } else {
                group.saturatedTicks = 0;
            }

            // desiredCount is not gated on the count: a client that has declared what it is about
            // to need is not guessing from a sample, and making it wait three ticks for instances
            // it has already said it wants is the latency declareExpectedConcurrency() exists to
            // avoid.
            const bool sustained = saturated && group.saturatedTicks >= _scaleUpSaturatedTicks;
            if ((sustained || running < group.desiredCount) && running < group.config.maxInstances) {
                auto svc = std::make_shared<Dto::ModuleProcess>();
                svc->config = group.config;
                group.instances.push_back(svc);
                toSpawn.push_back(svc);
                continue;
            }

            // Two reasons to give an instance back, and they are not the same question. The group
            // has gone quiet, or the pool is simply larger than the backlog now asks for. Only the
            // first was ever asked, and an application with a queue in front of it never goes
            // quiet - so a pool that grew during a burst stayed grown. The second reads the target
            // applyBacklog() maintains, which is why that had to start coming down.
            const bool idleLongEnough =
                    std::chrono::duration_cast<std::chrono::seconds>(now - group.lastActivityAt).count() >= _scaleDownIdleSeconds;
            const bool overProvisioned = running > group.desiredCount;

            if (idleCandidate && running > group.config.minInstances && (idleLongEnough || overProvisioned)) {
                // Gated on the GROUP's last activity, not just this instance's: round-robin can
                // leave one instance unpicked for a while even while its siblings stay busy, and
                // per-instance idle time alone can't tell "nobody wants this instance" apart from
                // "nobody wants any instance" - the former should NOT trigger a scale-down while
                // the group is still doing real work.
                // Idle in the sense that matters for scaling, but possibly still holding a
                // caller's connection open: a long poll does not count as load and would
                // otherwise be killed mid-wait, which the caller sees as "end of stream" rather
                // than as the empty answer it was about to get. The instance stays in the pool
                // until it is genuinely serving nothing; the next tick reconsiders it, and a
                // consumer polling continuously frees it in the gap between two polls.
                if (idleCandidate->inFlightRequests > 0) {
                    log_debug << "Not scaling down " << group.config.name << " yet, requests in flight: "
                              << idleCandidate->inFlightRequests;
                } else if (idleCandidate->backgroundTasks > 0) {
                    // Idle by every signal the manager has of its own, and not idle: an --async
                    // purge was answered long ago and is still running. Only the module can say
                    // so - see ModuleInstance::backgroundTasks - and stopping it here is what
                    // used to end the removal.
                    log_debug << "Not scaling down " << group.config.name << " yet, background tasks: "
                              << idleCandidate->backgroundTasks;
                } else {
                    std::erase(group.instances, idleCandidate);
                    toStop.push_back(idleCandidate);
                }
                // The group is genuinely idle now, so drop any earlier declared target back to the
                // floor - otherwise a one-off high-concurrency declaration would keep forcing the
                // pool back up forever even after that workload finished. Only on idleness: an
                // over-provisioned pool is shrinking towards a target that is already correct, and
                // slamming that target to the floor here would discard it and overshoot.
                if (idleLongEnough) group.desiredCount = group.config.minInstances;
            }
        }
    }

}// namespace Euclid::main