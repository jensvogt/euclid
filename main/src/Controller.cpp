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
#include <euclid/core/CryptoUtils.h>
#include <euclid/core/DateTimeUtils.h>
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/JwtUtils.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/ObjectCipher.h>
#include <euclid/database/Database.h>
#include <euclid/database/RepositoryFactory.h>
#include <euclid/dto/emm/EmmMapper.h>
#include <euclid/manager/Controller.h>
#include <euclid/manager/ControllerPlatform.h>
#include <euclid/manager/StartOrder.h>

// ControllerPlatform.h pulls in <sys/wait.h>/<csignal>/<unistd.h>/ on POSIX, and
// <windows.h>/<process.h>/<io.h> on Windows - so the rest of this file only needs to
// branch on _WIN32 for the handful of calls (fork/exec vs CreateProcess, kill vs
// TerminateProcess, waitpid vs GetExitCodeProcess) that don't have a shared name.

namespace Euclid::main {

    // The API gateway module. Named here because the shutdown ordering has to treat it as a
    // client of the applications rather than as one of the modules they depend on - see
    // stopOrder().
    constexpr auto kApiGateway = "eag";

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
        // does not have to be told about a restart it never noticed.
        if (const auto held = httpPorts().find(svc->instanceId); held != httpPorts().end()) return held->second;

        for (long port = first; port <= last; ++port) {
            const bool taken = std::ranges::any_of(httpPorts(), [port](const auto &entry) {
                return entry.second == static_cast<int>(port);
            });
            if (!taken) {
                httpPorts()[svc->instanceId] = static_cast<int>(port);
                return static_cast<int>(port);
            }
        }

        // Every port in the range is spoken for. Nothing is invented outside it: a port the
        // operator did not set aside might belong to something else on this host.
        log_warning << "No free HTTP port for " << svc->config.name << " in " << first << "-" << last;
        return 0;
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

#if defined(_WIN32)
    bool spawnInstance(const std::shared_ptr<Dto::ModuleProcess> &svc) {

        // Which pool slot this process is, so that anything it reports about itself can be
        // matched back to the instance the manager would start or stop. instanceId is the right
        // identifier rather than the pid: it is stable across restarts of the same slot, and it
        // is what emm_module.instances[] is already keyed by. Set on the instance's own copy of
        // the config - each ModuleProcess holds one - so both spawn paths pick it up from the
        // same place.
        svc->config.environment["EUCLID_INSTANCE_ID"] = svc->instanceId;

        // Only applications are given one - see allocateHttpPort(). An application binds it with
        // something like server.port=${EUCLID_HTTP_PORT:8080}, so the same artifact still runs
        // outside euclid on its own default.
        if (!svc->config.core) {
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
        svc->inFlightRequests = 0;
        svc->lastIdleAt = std::chrono::steady_clock::now();

        std::thread(drainPipe, outFd, false).detach();
        std::thread(drainPipe, errFd, true).detach();

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

        // Which pool slot this process is, so that anything it reports about itself can be
        // matched back to the instance the manager would start or stop. instanceId is the right
        // identifier rather than the pid: it is stable across restarts of the same slot, and it
        // is what emm_module.instances[] is already keyed by. Set on the instance's own copy of
        // the config - each ModuleProcess holds one - so both spawn paths pick it up from the
        // same place.
        svc->config.environment["EUCLID_INSTANCE_ID"] = svc->instanceId;

        // Only applications are given one - see allocateHttpPort(). An application binds it with
        // something like server.port=${EUCLID_HTTP_PORT:8080}, so the same artifact still runs
        // outside euclid on its own default.
        if (!svc->config.core) {
            if (const auto port = allocateHttpPort(svc); port > 0) {
                svc->httpPort = port;
                svc->config.environment["EUCLID_HTTP_PORT"] = std::to_string(port);
            }
        }
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

        std::thread(drainPipe, outPipe[0], false).detach();
        std::thread(drainPipe, errPipe[0], true).detach();

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

        const auto servers = Database::RepositoryFactory::instance().etsRepository()->listServers("");

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
            defined.insert(server.serverId);

            const bool wantRunning = server.desiredState == Database::Entity::ETS::TransferServerState::RUNNING;
            bool registered;
            {
                std::lock_guard lock(_mutex);
                registered = _services.contains(server.serverId);
            }

            if (wantRunning && !registered) {
                Dto::ModuleConfig config;
                config.name = server.serverId;
                config.executable = server.protocol == Database::Entity::ETS::TransferProtocol::FTP ? ftpExecutable : sftpExecutable;
                config.args = {"--config", configuration.filePath().string(), "--transfer-server", server.serverId};
                config.socketPath = socketDir + "/euclid-transfer-" + server.serverId + ".sock";
                config.maxRestarts = -1;
                config.autoRestart = true;
                // Deliberately single-instance: two processes cannot share a listening port, so
                // a transfer server is not something the autoscaler can scale out.
                config.minInstances = 1;
                config.maxInstances = 1;

                log_info << "Transfer server starting, serverId: " << server.serverId
                        << ", protocol: " << Database::Entity::ETS::TransferProtocolToString(server.protocol) << ", port: " << server.port;
                registerModule(config);
                start(server.serverId);

            } else if (!wantRunning && registered) {
                log_info << "Transfer server stopping, serverId: " << server.serverId;
                stop(server.serverId);
                deregisterModule(server.serverId);
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
        std::filesystem::path applicationDir(const std::string &applicationId) {
#ifdef _WIN32
            constexpr auto kDefaultDataDir = R"(C:\Program Files\euclid\data\application)";
#else
            constexpr auto kDefaultDataDir = "/usr/local/euclid/data/application";
#endif
            const auto dataDir = Core::Configuration::instance().getOr<std::string>("euclid.modules.eap.data-dir", kDefaultDataDir);
            return std::filesystem::path(dataDir) / applicationId;
        }

        // Copies the artifact out of ESM's object storage next to where the application will run.
        //
        // Read straight off ESM's data directory rather than through the module: the manager is
        // on the same host and already has the database, so going out over the gateway to fetch
        // bytes it can see would only add a dependency on ESM being up at spawn time. The object
        // row is still the source of truth for which file that is - a key is resolved to an
        // internal name only there.
        std::optional<std::filesystem::path> materializeArtifact(const Database::Entity::EAP::Application &application) {

            const auto object = Database::RepositoryFactory::instance().esmRepository()->findObjectByBucketAndKey(application.bucketErn, application.artifactKey);
            if (!object.has_value()) {
                log_error << "Application artifact not found, applicationId: " << application.applicationId << ", key: " << application.artifactKey;
                return std::nullopt;
            }

#ifdef _WIN32
            constexpr auto kDefaultStorageDir = R"(C:\Program Files\euclid\data\esm)";
#else
            constexpr auto kDefaultStorageDir = "/usr/local/euclid/data/esm";
#endif
            const auto storageDir = Core::Configuration::instance().getOr<std::string>("euclid.modules.esm.data-dir", kDefaultStorageDir);
            const auto source = std::filesystem::path(storageDir) / object->internalName;

            std::error_code ec;
            if (!std::filesystem::exists(source, ec)) {
                log_error << "Application artifact file missing, applicationId: " << application.applicationId << ", path: " << source.string();
                return std::nullopt;
            }

            const auto directory = applicationDir(application.applicationId);
            std::filesystem::create_directories(directory, ec);
            if (ec) {
                log_error << "Could not create application directory, path: " << directory.string() << ", error: " << ec.message();
                return std::nullopt;
            }

            const auto target = directory / std::filesystem::path(application.artifactKey).filename();

            // Re-copied whenever the local copy is not the object byte for byte, which is the case
            // that matters here: a new build uploaded to the same key. Compared by content rather
            // than by size, because a rebuild that happens to land on the same byte count - a
            // changed string literal of equal length is enough - would otherwise go on running the
            // previous build, with everything about the deployment looking correct.
            //
            // ESM computes md5Sum over the assembled object for both the single-shot and the
            // multipart path, so it is a plain content hash rather than an S3-style etag; size is
            // the fallback for an object stored before it was recorded.
            bool current = false;
            if (std::filesystem::exists(target, ec)) {
                try {
                    current = object->md5Sum.empty()
                                  ? static_cast<long>(std::filesystem::file_size(target, ec)) == object->size
                                  : Core::CryptoUtils::md5SumFile(target.string()) == object->md5Sum;
                } catch (const std::exception &e) {
                    // Unreadable for whatever reason - copying over it is the safe answer, and the
                    // copy reports its own failure.
                    log_warning << "Could not check the artifact already on disk, applicationId: " << application.applicationId
                            << ", path: " << target.string() << ", error: " << e.what();
                }
            }
            if (!current) {
                // An artifact stored in a bucket with encryption at rest is on disk as ciphertext,
                // and what has to end up here is something the runtime can exec() or hand to a JVM.
                // The object names the key it was written under (empty for one written in the
                // clear), so this decrypts exactly the artifacts that need it - and the md5Sum
                // compared above is over the plaintext either way, which is what makes the check
                // mean the same thing for both.
                if (object->encryptionKeyErn.empty()) {
                    std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, ec);
                    if (ec) {
                        log_error << "Could not materialize application artifact, applicationId: " << application.applicationId << ", error: " << ec.message();
                        return std::nullopt;
                    }
                } else {
                    const auto key = Database::RepositoryFactory::instance().ekmRepository()->findKeyByErn(object->encryptionKeyErn);
                    if (!key.has_value() || key->keyMaterial.empty()) {
                        log_error << "Application artifact is encrypted under a key that is gone, applicationId: " << application.applicationId
                                << ", keyErn: " << object->encryptionKeyErn;
                        return std::nullopt;
                    }
                    try {
                        Core::ObjectCipher::Transcode(Core::CryptoUtils::Base64Decode(key->keyMaterial), source, {}, target);
                    } catch (const std::exception &e) {
                        log_error << "Could not decrypt application artifact, applicationId: " << application.applicationId << ", error: " << e.what();
                        std::error_code partialEc;
                        std::filesystem::remove(target, partialEc);
                        return std::nullopt;
                    }
                }
                log_info << "Application artifact materialized, applicationId: " << application.applicationId << ", path: " << target.string()
                        << ", size: " << object->size;
            }

            // A BINARY artifact arrives as a plain object with no mode bits worth speaking of, so
            // it has to be made executable before anything can exec() it.
            if (application.runtime == Database::Entity::EAP::Runtime::BINARY) {
                std::filesystem::permissions(target, std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec,
                                             std::filesystem::perm_options::add, ec);
            }
            return target;
        }

        // How long an application's credentials are good for, and how much of that has to be left
        // before the reconciler bothers rewriting them. An hour is short enough that a token
        // lifted out of a process is worth little, and long enough that the rewrite is a rare
        // event rather than a per-request one.
        std::chrono::seconds credentialsTtl() {
            constexpr long kDefaultTtlSeconds = 3600;
            return std::chrono::seconds(std::max(60L, Core::Configuration::instance().getOr<long>("euclid.modules.eap.credentials-ttl-seconds", kDefaultTtlSeconds)));
        }

        std::filesystem::path credentialsPath(const std::string &applicationId) {
            return applicationDir(applicationId) / "credentials";
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
        // The namespace an application's own requests run in.
        //
        // Its own field when it has one. When it does not - every application deployed before
        // applications carried one, and anything created by a client that does not set it yet -
        // the application's EAM user is asked instead: a user granted exactly one namespace in
        // this account leaves no room for doubt about which was meant. Several, or none, and it
        // stays empty rather than guessing, which resolves names at the account root exactly as
        // before.
        std::string applicationNamespace(const Database::Entity::EAP::Application &application) {

            if (!application.nameSpace.empty()) return application.nameSpace;

            const auto user = Database::RepositoryFactory::instance().eamRepository()->findUserByUserId(application.userId);
            if (!user.has_value()) return {};

            for (const auto &grant: user->accountGrants) {
                if (grant.accountId != application.accountId) continue;
                if (grant.namespaces.size() == 1) return grant.namespaces.front();
                break;
            }
            return {};
        }

        bool writeApplicationCredentials(const Database::Entity::EAP::Application &application) {

            const auto expiresAt = std::chrono::system_clock::now() + credentialsTtl();
            const auto token = Core::JwtUtils::CreateToken(application.userId, Core::HttpActionServer::JwtSecret(), credentialsTtl());

            const auto &configuration = Core::Configuration::instance();
            const auto host = configuration.getOr<std::string>("euclid.gateway.http.host", "localhost");
            const auto port = configuration.getOr<long>("euclid.gateway.http.port", 5566);
            const auto scheme = configuration.getOr<bool>("euclid.gateway.tls.enabled", true) ? "https" : "http";

            const boost::json::value credentials = {
                    {"token", token},
                    {"expiresAt", Core::DateTimeUtils::ToISO8601(expiresAt)},
                    {"userId", application.userId},
                    {"accountId", application.accountId},
                    {"region", application.region},
                    // Sent by the client as x-euclid-namespace, which is what lets it name a
                    // queue, topic or bucket rather than spell out a full ERN.
                    {"namespace", applicationNamespace(application)},
                    {"endpoint", scheme + std::string("://") + host + ":" + std::to_string(port)},
            };

            const auto path = credentialsPath(application.applicationId);
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);

            // Written beside the target and moved into place, so a process reading the file never
            // sees half of one: rename() within a directory is atomic, a rewrite in place is not.
            const auto temporary = path.string() + ".new";
            {
                std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                if (!out) {
                    log_error << "Could not write application credentials, path: " << temporary;
                    return false;
                }
                out << boost::json::serialize(credentials);
            }
            std::filesystem::permissions(temporary, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                         std::filesystem::perm_options::replace, ec);
            std::filesystem::rename(temporary, path, ec);
            if (ec) {
                log_error << "Could not install application credentials, path: " << path.string() << ", error: " << ec.message();
                return false;
            }
            return true;
        }

        // Whether the credentials on disk are missing, unreadable, or close enough to expiry to be
        // worth replacing. Half the lifetime is the threshold, so an application always has at
        // least that long left in hand however unluckily a reconcile tick lands.
        bool credentialsNeedRefresh(const std::string &applicationId) {

            std::ifstream in(credentialsPath(applicationId));
            if (!in) return true;

            try {
                std::ostringstream buffer;
                buffer << in.rdbuf();
                const auto parsed = boost::json::parse(buffer.str());
                const auto expiresAt = Core::DateTimeUtils::FromISO8601(std::string(parsed.at("expiresAt").as_string()));
                return expiresAt - std::chrono::system_clock::now() < credentialsTtl() / 2;
            } catch (const std::exception &) {
                return true;
            }
        }

        // Everything the process needs to know about itself, and the credentials it needs to be
        // anyone. The application's own environment goes on first, so it cannot shadow these.
        std::map<std::string, std::string> applicationEnvironment(const Database::Entity::EAP::Application &application) {

            auto environment = application.environment;
            environment["EUCLID_APPLICATION_ID"] = application.applicationId;
            // Which version of the definition this process was started from. Also what the
            // reconciler compares to notice that the definition changed under a running pool -
            // see reconcileApplications().
            environment["EUCLID_APPLICATION_REVISION"] = Core::DateTimeUtils::ToISO8601(application.modified);
            // Which build this is, as the deployment named it. An application that logs this on
            // start-up answers "what is actually running?" from its own log, without anyone having
            // to compare checksums.
            environment["EUCLID_APPLICATION_VERSION"] = application.version;
            environment["EUCLID_APPLICATION_ERN"] = application.ern;
            environment["EUCLID_ACCOUNT_ID"] = application.accountId;
            environment["EUCLID_REGION"] = application.region;
            environment["EUCLID_USER_ID"] = application.userId;

            const auto &configuration = Core::Configuration::instance();
            const auto host = configuration.getOr<std::string>("euclid.gateway.http.host", "localhost");
            const auto port = configuration.getOr<long>("euclid.gateway.http.port", 5566);
            const auto scheme = configuration.getOr<bool>("euclid.gateway.tls.enabled", true) ? "https" : "http";
            const auto endpoint = scheme + std::string("://") + host + ":" + std::to_string(port);
            environment["EUCLID_ENDPOINT"] = endpoint;
            // The same value under the name the SDKs bind: euclid-spring reads EUCLID_BASE_URL
            // (euclid.base-url), and an application that finds neither concludes there is no
            // euclid to talk to. Two names for one endpoint is a small price for an application
            // that works whichever convention its framework follows.
            environment["EUCLID_BASE_URL"] = endpoint;

            // RFC 9421 rather than SigV4: an application is whatever language its author reached
            // for, and HTTP Message Signatures is the one of the two schemes with off-the-shelf
            // libraries in all of them.
            environment["EUCLID_SIGNATURE"] = "rfc9421";

            // Where the short-lived credentials are, and when the process should look again.
            environment["EUCLID_CREDENTIALS_FILE"] = credentialsPath(application.applicationId).string();

            const auto user = Database::RepositoryFactory::instance().eamRepository()->findUserByUserId(application.userId);

            // A technical principal deliberately gets no key: its long-lived secret stays in EAM,
            // and the process holds nothing but a token that expires. A user the caller named is
            // different - the operator owns that key and may well want an application signing
            // with it, so it is passed through as before.
            if (user.has_value() && user->loginEnabled && !user->accessKeys.empty()) {
                environment["EUCLID_ACCESS_KEY_ID"] = user->accessKeys.front().accessKeyId;
                environment["EUCLID_SECRET_ACCESS_KEY"] = user->accessKeys.front().secretAccessKey;
            } else if (!user.has_value()) {
                // Not fatal: an application that only serves requests never has to prove who it
                // is. One that calls back into euclid will get 401s, and this line is what
                // explains them.
                log_warning << "Application user not found, applicationId: " << application.applicationId << ", user: " << application.userId;
            }
            return environment;
        }

    }// namespace

    void ServiceController::reconcileApplications() {

        const auto applications = Database::RepositoryFactory::instance().eapRepository()->listApplications("");

        const auto &configuration = Core::Configuration::instance();
        const auto socketDir = configuration.getOr<std::string>("euclid.modules.eap.socket-dir", "/var/run/euclid");

        std::set<std::string> defined;

        for (const auto &application: applications) {
            defined.insert(application.applicationId);

            const bool wantRunning = application.desiredState == Database::Entity::EAP::ApplicationState::RUNNING;
            const auto revision = Core::DateTimeUtils::ToISO8601(application.modified);

            bool registered;
            std::string runningRevision;
            {
                std::lock_guard lock(_mutex);
                if (const auto it = _services.find(application.applicationId); it != _services.end()) {
                    registered = true;
                    if (const auto env = it->second.config.environment.find("EUCLID_APPLICATION_REVISION");
                        env != it->second.config.environment.end()) {
                        runningRevision = env->second;
                    }
                } else {
                    registered = false;
                }
            }

            // A definition that changed while its pool was running has to be picked up, and the
            // only way to pick it up is to start the processes again: an artifact, a command, an
            // environment and the credentials it was handed are all decided at spawn time.
            //
            // This is not a rare edge: deleting an application and creating it again between two
            // reconciles looks exactly like one that never changed, and the pool would keep
            // running with the previous definition's credentials - which by then have been
            // deleted along with the principal that owned them.
            if (wantRunning && registered && runningRevision != revision) {
                log_info << "Application definition changed, restarting, applicationId: " << application.applicationId
                        << ", revision: " << runningRevision << " -> " << revision;
                stop(application.applicationId);
                deregisterModule(application.applicationId);
                registered = false;
            }

            // Rewritten while the application runs, not only when it starts: that is the whole
            // point of putting them in a file. An instance started an hour ago is holding a token
            // that is about to expire, and the only thing that can replace it is this.
            if (wantRunning && credentialsNeedRefresh(application.applicationId)) {
                if (writeApplicationCredentials(application)) {
                    log_debug << "Application credentials refreshed, applicationId: " << application.applicationId;
                }
            }

            // An application is a client of the whole installation: it does not declare which
            // modules it calls, and the first thing many of them do is call one - a
            // @BucketListener subscribing to EES, say. Starting one while the modules are still
            // coming up means its first call meets a gateway that answers 503, or a module whose
            // socket exists but which is still opening its database connection - and a client
            // that treats that as "the server does not do this" degrades itself for the rest of
            // its life rather than retrying.
            //
            // So applications wait for the installation, and only then start. The wait is bounded
            // by the reconcile tick that runs this: a module that never comes up is reported by
            // its own supervision, and this simply keeps saying what it is waiting for.
            if (wantRunning && !registered && !modulesRunning()) {
                log_info << "Application waiting for the modules to come up, applicationId: " << application.applicationId;
                continue;
            }

            if (wantRunning && !registered) {

                const auto artifact = materializeArtifact(application);
                if (!artifact.has_value()) continue;

                // The runtime decides what actually gets exec'd: an interpreter with the artifact
                // as its argument, or the artifact itself. An application that spells out its own
                // command overrides all of it.
                Dto::ModuleConfig config;
                config.name = application.applicationId;
                const auto prefix = Database::Entity::EAP::RuntimeCommandPrefix(application.runtime);
                if (!application.command.empty()) {
                    config.executable = application.command;
                    config.args = {artifact->string()};
                } else if (!prefix.empty()) {
                    config.executable = prefix.front();
                    config.args.assign(prefix.begin() + 1, prefix.end());
                    config.args.push_back(artifact->string());
                } else {
                    config.executable = artifact->string();
                }
                for (const auto &argument: application.arguments) config.args.push_back(argument);

                config.environment = applicationEnvironment(application);
                config.workingDir = applicationDir(application.applicationId).string();
                config.socketPath = socketDir + "/euclid-application-" + application.applicationId + ".sock";
                config.readyTimeoutMs = static_cast<int>(application.readyTimeoutMs);
                config.maxRestarts = -1;
                config.autoRestart = true;
                // An application is supervised, not routed to: nothing asks it for anything over
                // a socket, so requiring it to create one only kills programs whose authors never
                // heard of the convention - see ModuleConfig::ReadinessCheck.
                config.readiness = Dto::ModuleConfig::ReadinessCheck::Liveness;
                config.minInstances = static_cast<int>(application.minInstances);
                config.maxInstances = static_cast<int>(application.maxInstances);

                log_info << "Application starting, applicationId: " << application.applicationId
                        << ", runtime: " << RuntimeToString(application.runtime) << ", command: " << config.executable
                        << ", instances: " << config.minInstances << "-" << config.maxInstances;
                registerModule(config);
                start(application.applicationId);

            } else if (!wantRunning && registered) {
                log_info << "Application stopping, applicationId: " << application.applicationId;
                stop(application.applicationId);
                deregisterModule(application.applicationId);
            }
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
        }
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

        // What the applications say about themselves. Nothing else can: a consumer application
        // receives no gateway request, so acquireInstance() never marks it busy and every pool of
        // them looks permanently idle to evaluateScaling().
        try {
            reconcileApplicationLoad();
        } catch (const std::exception &e) {
            log_error << "Application load reconcile failed, error: " << e.what();
        }

        // Both of the above only queue; this is the one that acts, so a thread change and a
        // restart asked for on the same tick cost one restart between them rather than two.
        rollQueuedInstance();
    }

    void ServiceController::reconcileApplicationLoad() {

        // Read once for every instance rather than once per instance: one query for each metric,
        // matched up by label below. A window rather than "the latest row" because EMO writes a
        // bucket per period - anything older than this has stopped reporting.
        // EMO's averaging period is what decides how old the newest row can be, so the window is
        // read from the same setting rather than guessed at here.
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
                newest.try_emplace(row.labelValue, row);
            }
            return newest;
        };

        const auto utilisation = latestByInstance("application-utilisation");
        const auto backlog = latestByInstance("application-backlog");
        if (utilisation.empty() && backlog.empty()) return;

        std::lock_guard lock(_mutex);
        for (auto &group: _services | std::views::values) {

            // Only pools whose instances report. A module served through the gateway already has a
            // truthful busy signal from acquireInstance(), and overwriting it from a metric that
            // arrives seconds late would be worse than what it has.
            bool reporting = false;
            long pending = 0;
            const auto now = std::chrono::steady_clock::now();

            for (auto &svc: group.instances) {
                if (svc->state != Database::Entity::ModuleState::RUNNING) continue;

                const auto sample = utilisation.find(svc->instanceId);
                if (sample == utilisation.end()) continue;
                reporting = true;

                // The peak within the bucket, not its average. A burst that saturates an instance
                // for twenty seconds and then stops averages down to almost nothing over a
                // five-minute bucket - which is exactly the load worth reacting to, reported as
                // though it never happened.
                if (sample->second.maxValue >= kBusyUtilisationPercent) {
                    svc->wasBusySinceLastCheck = true;
                    group.lastActivityAt = now;
                }

                if (const auto depth = backlog.find(svc->instanceId); depth != backlog.end()) {
                    pending += static_cast<long>(depth->second.maxValue);
                }
            }

            if (!reporting) continue;

            // An instance that has stopped reporting is not an idle instance - it is an instance
            // nothing is known about, and a crashed reporter would otherwise read as 0% and be
            // the first one stopped. Unknown counts as busy, so scale-down passes it over.
            for (auto &svc: group.instances) {
                if (svc->state != Database::Entity::ModuleState::RUNNING) continue;
                if (!utilisation.contains(svc->instanceId)) svc->wasBusySinceLastCheck = true;
            }

            // Work waiting that nobody is getting to is the one signal utilisation cannot give:
            // an instance is either busy or not, and "busy" says nothing about how much is left.
            // Raising desiredCount is how evaluateScaling() is asked for more, and it is bounded
            // there by maxInstances.
            if (pending >= kBacklogScaleUpMessages) {
                const auto running = std::ranges::count_if(group.instances, [](const auto &svc) {
                    return svc->state == Database::Entity::ModuleState::RUNNING;
                });
                if (const auto wanted = static_cast<int>(running) + 1; wanted > group.desiredCount) {
                    group.desiredCount = std::min(wanted, group.config.maxInstances);
                    log_info << "Application backlog, module: " << group.config.name << ", pending: " << pending
                             << ", desiredCount: " << group.desiredCount;
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

    void ServiceController::startAll() {

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
            for (const auto &server: Database::RepositoryFactory::instance().etsRepository()->listServers("")) {
                transferServers.insert(server.serverId);
            }
            for (const auto &application: Database::RepositoryFactory::instance().eapRepository()->listApplications("")) {
                applications.insert(application.applicationId);
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
            if ((saturated || running < group.desiredCount) && running < group.config.maxInstances) {
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
                // Idle in the sense that matters for scaling, but possibly still holding a
                // caller's connection open: a long poll does not count as load and would
                // otherwise be killed mid-wait, which the caller sees as "end of stream" rather
                // than as the empty answer it was about to get. The instance stays in the pool
                // until it is genuinely serving nothing; the next tick reconsiders it, and a
                // consumer polling continuously frees it in the gap between two polls.
                if (idleCandidate->inFlightRequests > 0) {
                    log_debug << "Not scaling down " << group.config.name << " yet, requests in flight: "
                              << idleCandidate->inFlightRequests;
                } else {
                    std::erase(group.instances, idleCandidate);
                    toStop.push_back(idleCandidate);
                }
                // The group is genuinely idle now, so drop any earlier declared target back to the
                // floor - otherwise a one-off high-concurrency declaration would keep forcing the
                // pool back up forever even after that workload finished.
                group.desiredCount = group.config.minInstances;
            }
        }
    }

}// namespace Euclid::main