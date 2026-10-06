// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <filesystem>
#include <fstream>
#include <string_view>
#include <utility>

// Boost includes
#include <boost/url.hpp>

#ifdef _WIN32
#include <io.h>
#include <thread>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <thread>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// The process environment, which a child is given in full - see WorkerClient::start. Declared
// rather than taken from <unistd.h>, which only declares it on glibc with _GNU_SOURCE.
extern char **environ;
#endif

// Euclid includes
#include <WorkerClient.h>
#include <euclid/core/ApplicationLaunch.h>
#include <euclid/core/Configuration.h>
#include <euclid/core/Version.h>
#include <euclid/core/CryptoUtils.h>
#include <euclid/core/DateTimeUtils.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/SystemUtils.h>
#ifdef _WIN32
// After everything else: it brings <windows.h>, which has to follow Boost.Asio's winsock headers.
#include <euclid/core/WindowsProcess.h>
#endif

namespace Euclid::Worker {

    namespace {

        // How long an instance asked to stop is given before it is killed.
        constexpr auto kStopGrace = std::chrono::seconds{10};

        std::string textField(const boost::json::value &value, const std::string &key) {
            if (!value.is_object()) return {};
            const auto *field = value.as_object().if_contains(key);
            return field != nullptr && field->is_string() ? std::string(field->as_string()) : std::string{};
        }

        // Reads lines from one of an instance's pipes until the instance closes it, recording each
        // as the manager records its own instances' - see Core::Launch::EmitOutput. Closes fd. A
        // CRT descriptor on Windows too (Core::WindowsProcess wraps the pipe handle in one), so the
        // only difference is the name of the call.
        void drainOutput(const int fd, const bool fromStderr, const std::string channel, const std::string fields) {
            std::string line;
            char buffer[4096];
#ifdef _WIN32
            int got;
            while ((got = _read(fd, buffer, sizeof(buffer))) > 0) {
#else
            ssize_t got;
            while ((got = read(fd, buffer, sizeof(buffer))) > 0 || (got < 0 && errno == EINTR)) {
#endif
                for (decltype(got) i = 0; i < got; ++i) {
                    if (buffer[i] == '\n') {
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        Core::Launch::EmitOutput(channel, line, fromStderr, fields);
                        line.clear();
                    } else {
                        line += buffer[i];
                    }
                }
            }
            if (!line.empty()) Core::Launch::EmitOutput(channel, line, fromStderr, fields);
#ifdef _WIN32
            _close(fd);
#else
            close(fd);
#endif
        }

        long numberField(const boost::json::value &value, const std::string &key) {
            if (!value.is_object()) return 0;
            const auto *field = value.as_object().if_contains(key);
            if (field == nullptr) return 0;
            try {
                return field->to_number<long>();
            } catch (const std::exception &) {
                return 0;
            }
        }

    }// namespace

    WorkerClient::WorkerClient(Options options, CLI::Credentials::Entry credentials)
        : _options(std::move(options)), _credentials(std::move(credentials)),
          _client(_options.endpoint, _credentials, _options.caCertPath) {}

    std::filesystem::path WorkerClient::applicationDir(const std::string &runtimeName) const {
        return std::filesystem::path(_options.dataDir) / "applications" / runtimeName;
    }

    CLI::HttpResponse WorkerClient::post(const std::string &target, const std::string &action,
                                         const boost::json::value &body) const {
        try {
            return _client.Post(target, action, body);
        } catch (const std::exception &e) {
            log_warning << "Could not reach the master, action: " << action << ", error: " << e.what();
            return CLI::HttpResponse{.statusCode = 0, .body = {}};
        }
    }

    Core::Artifact::Call WorkerClient::artifactTransport() const {

        return [this](const std::string &action,
                      const std::vector<std::pair<std::string, std::string> > &headers,
                      const std::string &body) -> Core::ModuleClient::ModuleResponse {
            // Two shapes, because two of ESM's four object actions answer with bytes rather than
            // JSON - get-object and download-part - and the client has a separate call for each.
            // Which is which is ESM's contract, not something to infer from what came back.
            try {
            if (action == "get-object" || action == "download-part") {
                const auto response = _client.PostForBinary("esm", action, headers);

                // `data` on success and `errorBody` on failure - a successful response's body is
                // the object's bytes and there is nothing to parse it into. Carried across so that
                // ModuleResponse::describe() still has the module's own explanation to print,
                // which is the whole reason a refused call is diagnosable.
                return {.status = response.statusCode,
                        .body = response.statusCode / 100 == 2 ? response.data
                                                               : boost::json::serialize(response.errorBody)};
            }

            const auto response = _client.Post("esm", action, boost::json::parse(body.empty() ? "{}" : body), headers);
            return {.status = response.statusCode, .body = boost::json::serialize(response.body)};

            } catch (const std::exception &e) {
                // Status 0 - never got that far - which Core::Artifact reports as a failed fetch.
                // A slot that cannot be fetched does not start; it is not a reason for the worker
                // to die and orphan everything else it is running.
                log_warning << "Could not reach ESM, action: " << action << ", error: " << e.what();
                return {};
            }
        };
    }

    std::optional<std::chrono::seconds> WorkerClient::Register() {

        boost::json::object labels;
        for (const auto &[key, value]: _options.labels) labels[key] = value;

        // Found out again on every registration rather than once at start-up: a worker that
        // re-registers after losing the master may well have lost it because its address changed.
        auto address = _options.address;
        if (address.empty()) {
            if (const auto parsed = boost::urls::parse_uri(_options.endpoint); parsed) {
                const auto port = parsed->has_port() ? std::string(parsed->port()) : (parsed->scheme() == "http" ? "80" : "443");
                address = Core::SystemUtils::GetOutboundAddress(std::string(parsed->host_address()), port).value_or("");
            }
            if (address.empty()) log_warning << "Could not determine this node's address, endpoint: " << _options.endpoint;
        }

        const boost::json::value body{
                {"node", _options.nodeName},
                {"address", address},
                {"labels", labels},
                {"cpuCount", static_cast<long>(std::max(1U, std::thread::hardware_concurrency()))},
                {"version", APP_VERSION},
                {"os", Core::SystemUtils::GetOperatingSystem()},
                {"arch", Core::SystemUtils::GetArchitecture()},
        };

        const auto response = post("eap", "register-node", body);
        if (response.statusCode / 100 != 2) {
            log_error << "Could not register this node, node: " << _options.nodeName
                      << ", status: " << response.statusCode << ", body: " << boost::json::serialize(response.body);
            return std::nullopt;
        }

        _address = address;

        const auto lease = numberField(response.body, "leaseSeconds");
        log_info << "Node registered, node: " << _options.nodeName << ", address: " << address << ", lease: " << lease << "s";
        return std::chrono::seconds{lease > 0 ? lease : 45};
    }

    bool WorkerClient::Renew(std::vector<Reconciler::Assignment> &assigned,
                             std::chrono::system_clock::time_point &leaseExpiresAt) {

        // This machine's own one-minute load average, which only this machine can read. Sent with
        // the renewal because placement's third tie-break reads it, and because the renewal is
        // already the one call this worker makes every tick.
        //
        // Zero where there is nothing to read - /proc/loadavg is Linux-only - which reads as idle
        // at the far end. Optimistic, and the right direction: the figure only ever breaks a tie
        // between nodes already running the same number of instances of the application.
        const auto load = Core::SystemUtils::ReadLoadAverage();

        const boost::json::value body{{"node", _options.nodeName},
                                      {"loadAverage", load.has_value() ? load->oneMinute : 0.0}};

        const auto response = post("eap", "renew-node", body);

        if (response.statusCode == 404) {
            // The master does not know this node. Worth distinguishing from a failure to reach it:
            // the answer is to register again, not to wait out the lease.
            log_warning << "This node is not registered with the master; registering again";
            std::ignore = Register();
            return false;
        }
        if (response.statusCode / 100 != 2) {
            // Not a reason to stop anything - that is what the lease is for. Logged at warning
            // because a worker that cannot renew is on a clock.
            log_warning << "Renewal failed, node: " << _options.nodeName << ", status: " << response.statusCode;
            return false;
        }

        assigned.clear();
        if (const auto *instances = response.body.is_object() ? response.body.as_object().if_contains("instances") : nullptr;
            instances != nullptr && instances->is_array()) {

            for (const auto &entry: instances->as_array()) {
                Reconciler::Assignment assignment;
                assignment.instanceId = textField(entry, "instanceId");
                assignment.runtimeName = textField(entry, "runtimeName");
                assignment.applicationId = textField(entry, "applicationId");
                assignment.revision = textField(entry, "revision");
                assignment.bucketErn = textField(entry, "bucketErn");
                assignment.artifactKey = textField(entry, "artifactKey");
                assignment.artifactSize = numberField(entry, "artifactSize");
                assignment.md5Sum = textField(entry, "md5Sum");
                assignment.runtime = textField(entry, "runtime");
                assignment.command = textField(entry, "command");
                assignment.accountId = textField(entry, "accountId");
                assignment.nameSpace = textField(entry, "nameSpace");
                assignment.logLevel = textField(entry, "logLevel");

                if (const auto *arguments = entry.is_object() ? entry.as_object().if_contains("arguments") : nullptr;
                    arguments != nullptr && arguments->is_array()) {
                    for (const auto &argument: arguments->as_array()) {
                        if (argument.is_string()) assignment.arguments.emplace_back(argument.as_string());
                    }
                }
                if (const auto *environment = entry.is_object() ? entry.as_object().if_contains("environment") : nullptr;
                    environment != nullptr && environment->is_object()) {
                    for (const auto &[name, value]: environment->as_object()) {
                        if (value.is_string()) assignment.environment[std::string(name)] = std::string(value.as_string());
                    }
                }
                if (!assignment.instanceId.empty()) assigned.push_back(std::move(assignment));
            }
        }

        if (const auto expiry = textField(response.body, "leaseExpiresAt"); !expiry.empty()) {
            leaseExpiresAt = Core::DateTimeUtils::FromISO8601(expiry);
        }

        applyLogLevels(assigned);

        // A crash is only remembered for as long as the slot is ours: one re-placed elsewhere, or
        // removed, starts from nothing if it ever comes back.
        std::erase_if(_crashes, [&assigned](const auto &entry) {
            return std::ranges::none_of(assigned, [&entry](const Reconciler::Assignment &a) { return a.instanceId == entry.first; });
        });

        // A port goes back once its slot is neither assigned here nor still running - the stop
        // that follows an unassignment is what frees it, not the renewal that announced it.
        std::erase_if(_ports, [this, &assigned](const auto &entry) {
            return !_instances.contains(entry.first)
                   && std::ranges::none_of(assigned, [&entry](const Reconciler::Assignment &a) { return a.instanceId == entry.first; });
        });

        log_debug << "Renewed, node: " << _options.nodeName << ", assigned: " << assigned.size();
        return true;
    }

    std::optional<std::chrono::system_clock::time_point>
    WorkerClient::writeCredentials(const Reconciler::Assignment &assignment) const {

        // Asked for, never minted. A worker holding the signing secret could mint this itself for
        // any principal in the installation; main() refuses to start if it finds that secret in
        // the configuration. docs/worker-nodes.md §3.2.
        const auto issuedAt = std::chrono::system_clock::now();

        const auto response = post("eap", "issue-instance-credentials",
                                   boost::json::value{{"node", _options.nodeName},
                                                              {"instanceId", assignment.instanceId},
                                                              {"runtimeName", assignment.runtimeName}});
        if (response.statusCode / 100 != 2) {
            log_error << "Could not get credentials for instance " << assignment.instanceId
                      << ", status: " << response.statusCode << ", body: " << boost::json::serialize(response.body);
            return std::nullopt;
        }

        // The endpoint in the blob is the gateway as the master reaches it - "localhost", most
        // likely - which from here is this machine. Replaced with the one this worker reaches it
        // on, which is the one an application on this host can.
        auto credentials = response.body;
        if (credentials.is_object()) credentials.as_object()["endpoint"] = _options.endpoint;

        // The same file, the same name and the same mode as the manager writes for its own
        // applications - one function writes both.
        if (!Core::Launch::WriteCredentials(applicationDir(assignment.runtimeName) / Core::Launch::CredentialsFileName, credentials)) {
            return std::nullopt;
        }

        // Halfway through their life, which is the rule the manager follows too. Derived from the
        // blob's own expiry rather than a configured TTL: the TTL is the master's setting, and a
        // copy of it here would only be a second place for the two to disagree.
        //
        // A blob with no readable expiry falls back to asking again on the next tick. Noisy, and
        // the alternative is an instance whose token quietly expires - which looks like the
        // application being broken rather than its credentials being stale.
        const auto expiresAt = Core::Launch::CredentialsExpiry(response.body);
        if (!expiresAt.has_value()) {
            log_warning << "Credentials for instance " << assignment.instanceId << " carry no readable expiry; will ask again next tick";
            return issuedAt;
        }
        if (*expiresAt <= issuedAt) {
            log_warning << "Credentials for instance " << assignment.instanceId << " are already expired";
        }
        return Core::Launch::RefreshAt(*expiresAt, issuedAt);
    }

    void WorkerClient::RefreshCredentials() {

        const auto now = std::chrono::system_clock::now();

        for (auto &[instanceId, instance]: _instances) {

            if (instance.credentialsRefreshAt.time_since_epoch().count() != 0 && now < instance.credentialsRefreshAt) {
                continue;
            }

            if (const auto next = writeCredentials(instance.assignment); next.has_value()) {
                instance.credentialsRefreshAt = *next;
                log_debug << "Credentials refreshed for instance " << instanceId;
            } else {
                // Not fatal and not a reason to stop the instance: it is still holding a token
                // with at least half its life left, which is the whole point of refreshing at the
                // half-way mark rather than at the end. Tried again on the next tick.
                log_warning << "Could not refresh credentials for instance " << instanceId << "; will try again";
            }
        }
    }

    bool WorkerClient::start(const Reconciler::Assignment &assignment) {

        const auto directory = applicationDir(assignment.runtimeName);
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);

        const auto target = directory / std::filesystem::path(assignment.artifactKey).filename();

        // Fetched only when the copy on disk is not the object byte for byte, which is the reason
        // a restart is cheap and a redeploy is not missed. The manager's rule, against the md5 and
        // size the master sends from ESM's row - see Core::Launch::ArtifactIsCurrent.
        if (!Core::Launch::ArtifactIsCurrent(target, assignment.md5Sum, assignment.artifactSize)) {
            const Core::Artifact::Request fetch{.bucketErn = assignment.bucketErn,
                                                .key = assignment.artifactKey,
                                                .size = assignment.artifactSize,
                                                .accountId = _credentials.accountId,
                                                .nameSpace = _credentials.nameSpace,
                                                .region = _credentials.region};

            if (!Core::Artifact::Download(fetch, target, artifactTransport())) {
                log_error << "Could not fetch the artifact for instance " << assignment.instanceId;
                return false;
            }
        }

        Core::Launch::PrepareArtifact(target, assignment.runtime, assignment.command);

        const auto credentialsRefreshAt = writeCredentials(assignment);
        if (!credentialsRefreshAt.has_value()) return false;

        // Everything about the process is decided here, the same on both platforms; only starting
        // it differs, below.
        //
        // Built before any fork: the child should do nothing but exec, and reading the
        // configuration takes a lock the parent may have been holding when it forked. Absolute,
        // because the child starts in the application's directory, and a worker started with a
        // relative --data-dir would otherwise exec a path that no longer leads anywhere.
        //
        // Where this node keeps each interpreter is euclid.worker.runtimes.<key>. JAVA also still
        // reads euclid.worker.java, the name every worker configured before the others existed
        // uses; the newer key wins where both are set.
        const auto artifactPath = std::filesystem::absolute(target, ec);
        const auto commandLine = Core::Launch::CommandLine(
                assignment.runtime, assignment.command, (ec ? target : artifactPath).string(), assignment.arguments,
                [](const std::string &key, const std::string &fallback) {
                    const auto &configuration = Core::Configuration::instance();
                    const auto legacy = key == "java" ? configuration.getOr<std::string>("euclid.worker.java", fallback) : fallback;
                    return configuration.getOr<std::string>("euclid.worker.runtimes." + key, legacy);
                });
        const auto workingDir = directory.string();
        // Absolute for the same reason: the application reads it after the chdir.
        const auto credentialsPath = std::filesystem::absolute(directory / Core::Launch::CredentialsFileName, ec).string();

        // The environment: the definition's half as the master sent it, then this host's half -
        // the gateway as this worker reaches it, the certificate this worker was told to trust for
        // it, and the credentials file - exactly as the manager assembles it for its own.
        auto environment = assignment.environment;
        std::string caCertPath;
        if (!_options.caCertPath.empty() && std::filesystem::exists(_options.caCertPath, ec)) {
            caCertPath = std::filesystem::absolute(_options.caCertPath, ec).string();
        }
        Core::Launch::AddHostEnvironment(environment, _options.endpoint, caCertPath, credentialsPath);
        environment["EUCLID_INSTANCE_ID"] = assignment.instanceId;

        // A port of its own for the instance's HTTP listener, from the range this node set aside,
        // under the name the manager uses for its own instances. None configured, none given - as
        // on the manager - and then the gateway has nothing to route to here.
        const auto &configuration = Core::Configuration::instance();
        const auto httpPort = Core::Launch::PickHttpPort(_ports, assignment.instanceId,
                                                         configuration.getOr<long>("euclid.worker.http-port-min", 0),
                                                         configuration.getOr<long>("euclid.worker.http-port-max", 0));
        if (httpPort > 0) {
            _ports[assignment.instanceId] = httpPort;
            environment["EUCLID_HTTP_PORT"] = std::to_string(httpPort);
        } else if (configuration.getOr<long>("euclid.worker.http-port-min", 0) > 0) {
            log_warning << "No free HTTP port for instance " << assignment.instanceId << " in euclid.worker.http-port-min/max";
        }

        const auto channel = Core::Launch::OutputChannel(assignment.runtimeName);
        const auto fields = Core::Launch::OutputFields(assignment.runtimeName, assignment.nameSpace, assignment.accountId);

        Instance instance{.assignment = assignment, .httpPort = httpPort,
                          .executable = commandLine.front(), .credentialsRefreshAt = *credentialsRefreshAt};

#ifdef _WIN32
        // The manager's own Windows spawn - see Core::WindowsProcess: the two pipe ends and nothing
        // else inherited, started suspended into a job that dies with this worker, a stop event
        // named in EUCLID_STOP_EVENT, and this environment layered over the worker's. The job is
        // what makes a killed worker safe: its instances go with it, rather than running on while
        // the master re-places the same slots elsewhere once the lease runs out.
        const auto spawned = Core::WindowsProcess::Spawn(assignment.runtimeName, commandLine, environment, workingDir);
        if (!spawned.has_value()) return false;

        instance.pid = static_cast<int>(spawned->pid);
        instance.processHandle = spawned->process;
        instance.stopEvent = spawned->stopEvent;
        std::thread(drainOutput, spawned->stdoutFd, false, channel, fields).detach();
        std::thread(drainOutput, spawned->stderrFd, true, channel, fields).detach();
#else
        // Inherited, then overridden, as the manager's spawn does - but assembled here, before the
        // fork, so the child has nothing to allocate: setenv() in a child of a multi-threaded
        // process can deadlock on a lock some other thread held at the moment of the fork.
        std::vector<std::string> environmentStrings;
        for (char **entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
            const std::string_view pair(*entry);
            if (!environment.contains(std::string(pair.substr(0, pair.find('='))))) environmentStrings.emplace_back(pair);
        }
        for (const auto &[name, value]: environment) environmentStrings.push_back(name + "=" + value);

        std::vector<char *> envp;
        for (auto &pair: environmentStrings) envp.push_back(pair.data());
        envp.push_back(nullptr);

        std::vector<char *> argv;
        for (const auto &argument: commandLine) argv.push_back(const_cast<char *>(argument.c_str()));
        argv.push_back(nullptr);

        // The instance's stdout and stderr, captured and re-emitted on the application's channel as
        // the manager does for its own - rather than inherited, which wrote them raw into the
        // worker's own output, on no channel, unsanitised, at no severity, and under a Windows
        // service nowhere at all. Close-on-exec on every end, so no later child inherits a pipe
        // and holds it open past this instance's exit; dup2() clears it on the two the child keeps.
        int outPipe[2] = {-1, -1};
        int errPipe[2] = {-1, -1};
        if (pipe(outPipe) != 0 || pipe(errPipe) != 0) {
            log_error << "Could not create output pipes for instance " << assignment.instanceId << ": " << strerror(errno);
            for (const int fd: {outPipe[0], outPipe[1], errPipe[0], errPipe[1]}) {
                if (fd >= 0) close(fd);
            }
            return false;
        }
        for (const int fd: {outPipe[0], outPipe[1], errPipe[0], errPipe[1]}) fcntl(fd, F_SETFD, FD_CLOEXEC);

        const auto pid = fork();
        if (pid < 0) {
            log_error << "Could not fork for instance " << assignment.instanceId << ": " << strerror(errno);
            for (const int fd: {outPipe[0], outPipe[1], errPipe[0], errPipe[1]}) close(fd);
            return false;
        }

        if (pid == 0) {
            // The child. Its own process group, so stopping it does not depend on the worker's own
            // signal disposition and a kill reaches whatever it started.
            setpgid(0, 0);

            dup2(outPipe[1], STDOUT_FILENO);
            dup2(errPipe[1], STDERR_FILENO);

            // The application's own directory, as the manager starts its applications in: the one
            // place under ProtectSystem=strict an application can write a relative path to.
            if (chdir(workingDir.c_str()) != 0) _exit(126);

            // Swapped in rather than built: execvp searches the PATH of the environment it finds
            // here, which is the one the application is meant to have.
            environ = envp.data();
            execvp(argv.front(), argv.data());

            // Only reachable when exec failed. _exit rather than exit: this is a forked child of a
            // process holding log sinks and sockets, and running their destructors here would
            // flush and close things the parent still owns.
            _exit(127);
        }

        // The parent keeps the read ends and drains them until the child closes its side - which
        // it does on exit, so the threads end with the instance. Detached, and given copies of
        // everything they use, so nothing they touch belongs to this object.
        close(outPipe[1]);
        close(errPipe[1]);
        std::thread(drainOutput, outPipe[0], false, channel, fields).detach();
        std::thread(drainOutput, errPipe[0], true, channel, fields).detach();

        instance.pid = static_cast<int>(pid);
#endif

        instance.startedAt = std::chrono::system_clock::now();
        const auto startedPid = instance.pid;
        _instances[assignment.instanceId] = std::move(instance);

        log_info << "Started instance " << assignment.instanceId << ", application: " << assignment.applicationId
                 << ", pid: " << startedPid << ", port: " << httpPort << ", command: " << commandLine.front();

        report(assignment, startedPid, httpPort, "RUNNING");
        return true;
    }

    void WorkerClient::askToStop(const Instance &instance) {
#ifdef _WIN32
        // The stop event - SIGTERM's counterpart, see Core::WindowsProcess::RequestStop. A JVM does
        // not watch it, so in practice an application there is ended by the kill once the grace
        // period is over, as one the manager runs on Windows is.
        Core::WindowsProcess::RequestStop(static_cast<HANDLE>(instance.stopEvent));
#else
        // The group, not the process: a JVM that started helpers would otherwise leave them behind,
        // and they would go on consuming the queue this instance was reading.
        if (instance.pid > 0) kill(-instance.pid, SIGTERM);
#endif
    }

    void WorkerClient::stop(const Reconciler::Running &running, const std::chrono::steady_clock::time_point deadline) {

        const auto found = _instances.find(running.instanceId);
        if (found == _instances.end()) return;

        askToStop(found->second);

#ifdef _WIN32
        if (const auto process = static_cast<HANDLE>(found->second.processHandle); process) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (WaitForSingleObject(process, static_cast<DWORD>(std::max<long long>(0, left.count()))) != WAIT_OBJECT_0) {
                log_warning << "Instance " << running.instanceId << " did not stop, killing pid " << found->second.pid;
                Core::WindowsProcess::Kill(process);
                WaitForSingleObject(process, 5000);
            }
            CloseHandle(process);
        }
        if (found->second.stopEvent) CloseHandle(static_cast<HANDLE>(found->second.stopEvent));
#else
        if (const auto pid = found->second.pid; pid > 0) {
            bool exited = false;
            while (!exited && std::chrono::steady_clock::now() < deadline) {
                exited = waitpid(pid, nullptr, WNOHANG) != 0;
                if (!exited) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!exited && waitpid(pid, nullptr, WNOHANG) == 0) {
                log_warning << "Instance " << running.instanceId << " did not stop, killing pid " << pid;
                kill(-pid, SIGKILL);
                std::ignore = waitpid(pid, nullptr, 0);
            }
        }
#endif

        log_info << "Stopped instance " << running.instanceId;
        report(found->second.assignment, -1, 0, "STOPPED");
        _instances.erase(found);
    }

    void WorkerClient::report(const Reconciler::Assignment &assignment, const int pid, const int httpPort,
                              const std::string &state) const {

        const auto response = post("eap", "report-node-instance",
                                   boost::json::value{{"node", _options.nodeName},
                                                              {"instanceId", assignment.instanceId},
                                                              {"runtimeName", assignment.runtimeName},
                                                              // The address this node registered with
                                                              // rather than its host name: the
                                                              // gateway connects to this, and a host
                                                              // name a worker knows itself by is not
                                                              // one the master's DNS need know.
                                                              {"host", _address.empty() ? Core::SystemUtils::GetHostName() : _address},
                                                              {"pid", pid},
                                                              {"httpPort", httpPort},
                                                              {"state", state}});

        // Advisory. A report that could not be written costs the master one tick of accuracy, and
        // the process is running - or stopped - either way. A 409 is the one worth saying out loud:
        // it means this slot is no longer ours, which the next renewal will say too.
        if (response.statusCode == 409) {
            log_warning << "The master says instance " << assignment.instanceId << " is not ours any more";
        } else if (response.statusCode / 100 != 2) {
            log_debug << "Could not report instance " << assignment.instanceId << ", status: " << response.statusCode;
        }
    }

    void WorkerClient::Apply(const Reconciler::Plan &plan) {

        if (plan.leaseLost) {
            // Said at warning, and said as what it is. An operator looking at a worker that shut
            // down needs to know whether it was told to or whether it lost contact, and those have
            // very different answers.
            log_warning << "Lease expired without a renewal; stopping everything this node runs. "
                        << "The master will place this work elsewhere.";
        }

        // Stops first. A revision change is a stop and a start of the same slot, and starting the
        // replacement before the old one is gone would have two processes on one slot for as long
        // as the stop takes - which is the thing this whole design is about.
        for (const auto &running: plan.stop) stop(running, std::chrono::steady_clock::now() + kStopGrace);
        for (const auto &assignment: plan.start) std::ignore = start(assignment);
    }

    void WorkerClient::applyLogLevels(const std::vector<Reconciler::Assignment> &assigned) {

        // What each application on this node should be held at now. One entry per application,
        // however many of its instances are here: the channel is the application's, not the slot's.
        std::map<std::string, std::string> wanted;
        for (const auto &assignment: assigned) wanted[Core::Launch::OutputChannel(assignment.runtimeName)] = assignment.logLevel;

        for (const auto &[channel, level]: wanted) {
            const auto applied = _channelLevels.find(channel);
            if (applied != _channelLevels.end() && applied->second == level) continue;

            // New here and asking for nothing in particular: the configuration already applies.
            if (applied == _channelLevels.end() && level.empty()) {
                _channelLevels[channel] = level;
                continue;
            }

            if (level.empty()) {
                Core::LogStream::ClearChannelSeverity(channel);
            } else if (!Core::LogStream::SetChannelSeverity(channel, level)) {
                // The master validated it when it was set, so this is a level name the two sides
                // disagree about - worth a line, and the channel is left as it was.
                log_warning << "Could not apply log level '" << level << "' to " << channel;
                continue;
            }
            log_info << "Log level for " << channel << ": " << (level.empty() ? "(configuration)" : level);
            _channelLevels[channel] = level;
        }

        // An application no longer on this node goes back under the configuration, so its level
        // does not linger and apply to whatever is placed here next under that name.
        for (auto it = _channelLevels.begin(); it != _channelLevels.end();) {
            if (wanted.contains(it->first)) {
                ++it;
                continue;
            }
            Core::LogStream::ClearChannelSeverity(it->first);
            it = _channelLevels.erase(it);
        }
    }

    void WorkerClient::Reap() {

        const auto now = std::chrono::system_clock::now();

        for (auto it = _instances.begin(); it != _instances.end();) {

            std::string how;
#ifdef _WIN32
            const auto process = static_cast<HANDLE>(it->second.processHandle);
            const auto exitCode = Core::WindowsProcess::ExitCode(process);
            if (!exitCode.has_value()) {
                ++it;
                continue;
            }
            how = "exit code " + std::to_string(*exitCode);
            CloseHandle(process);
            if (it->second.stopEvent) CloseHandle(static_cast<HANDLE>(it->second.stopEvent));
#else
            int status = 0;
            if (it->second.pid <= 0 || waitpid(it->second.pid, &status, WNOHANG) != it->second.pid) {
                ++it;
                continue;
            }

            // How it ended, in the words an operator would use: an exit code, or the signal that
            // took it. 127 is the child's own _exit after an exec that failed - a missing
            // interpreter, most often - which is worth naming, since nothing else will.
            if (WIFEXITED(status)) {
                how = "exit code " + std::to_string(WEXITSTATUS(status));
                if (WEXITSTATUS(status) == 127) how += " (" + it->second.executable + " could not be executed)";
                if (WEXITSTATUS(status) == 126) how += " (could not enter the application directory)";
            } else if (WIFSIGNALED(status)) {
                how = "signal " + std::to_string(WTERMSIG(status));
            } else {
                how = "status " + std::to_string(status);
            }
#endif

            const auto ranFor = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.startedAt);
            const auto previous = _crashes.contains(it->first) ? _crashes.at(it->first).delay : std::chrono::seconds{0};
            const auto delay = Reconciler::RestartDelay(previous, ranFor);
            _crashes[it->first] = Crash{.delay = delay, .notBefore = now + delay};

            log_warning << "Instance " << it->first << " exited on its own, application: " << it->second.assignment.applicationId
                        << ", " << how << ", ran for " << ranFor.count() << "s; restarting in " << delay.count() << "s";

            report(it->second.assignment, -1, 0, "CRASHED");
            it = _instances.erase(it);
        }
    }

    std::map<std::string, std::chrono::system_clock::time_point> WorkerClient::HoldOff() const {
        std::map<std::string, std::chrono::system_clock::time_point> holdOff;
        for (const auto &[instanceId, crash]: _crashes) holdOff[instanceId] = crash.notBefore;
        return holdOff;
    }

    std::vector<Reconciler::Running> WorkerClient::Running() const {

        std::vector<Reconciler::Running> running;
        running.reserve(_instances.size());

        for (const auto &[instanceId, instance]: _instances) {
            running.push_back(Reconciler::Running{.instanceId = instanceId,
                                                  .runtimeName = instance.assignment.runtimeName,
                                                  .revision = instance.assignment.revision});
        }
        return running;
    }

    void WorkerClient::StopAll() {

        // Everybody asked first, then one grace period shared between them: stopped one after
        // another, a node with five instances that ignore the request - every JVM on Windows -
        // would take five grace periods to stop, well past what a service is given to stop in.
        for (const auto &[instanceId, instance]: _instances) askToStop(instance);

        const auto deadline = std::chrono::steady_clock::now() + kStopGrace;
        for (const auto &running: Running()) stop(running, deadline);
    }

}// namespace Euclid::Worker
