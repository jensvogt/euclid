// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <filesystem>
#include <fstream>
#include <utility>

#ifndef _WIN32
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

// Euclid includes
#include <WorkerClient.h>
#include <euclid/core/Configuration.h>
#include <euclid/core/Version.h>
#include <euclid/core/CryptoUtils.h>
#include <euclid/core/DateTimeUtils.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/SystemUtils.h>

namespace Euclid::Worker {

    namespace {

        std::string textField(const boost::json::value &value, const std::string &key) {
            if (!value.is_object()) return {};
            const auto *field = value.as_object().if_contains(key);
            return field != nullptr && field->is_string() ? std::string(field->as_string()) : std::string{};
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

        const boost::json::value body{
                {"node", _options.nodeName},
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

        const auto lease = numberField(response.body, "leaseSeconds");
        log_info << "Node registered, node: " << _options.nodeName << ", lease: " << lease << "s";
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
                assignment.runtime = textField(entry, "runtime");

                if (const auto *arguments = entry.is_object() ? entry.as_object().if_contains("arguments") : nullptr;
                    arguments != nullptr && arguments->is_array()) {
                    for (const auto &argument: arguments->as_array()) {
                        if (argument.is_string()) assignment.arguments.emplace_back(argument.as_string());
                    }
                }
                if (!assignment.instanceId.empty()) assigned.push_back(std::move(assignment));
            }
        }

        if (const auto expiry = textField(response.body, "leaseExpiresAt"); !expiry.empty()) {
            leaseExpiresAt = Core::DateTimeUtils::FromISO8601(expiry);
        }

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

        const auto directory = applicationDir(assignment.runtimeName);
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);

        // Written beside the target and renamed, which is what the manager does and for the same
        // reason: a process reading this file must never see half of one.
        const auto path = directory / "credentials.json";
        const auto temporary = path.string() + ".new";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            if (!out) {
                log_error << "Could not write credentials, path: " << temporary;
                return std::nullopt;
            }
            out << boost::json::serialize(response.body);
        }
        std::filesystem::rename(temporary, path, ec);
        if (ec) {
            log_error << "Could not move credentials into place, path: " << path.string() << ", error: " << ec.message();
            return std::nullopt;
        }

        // Halfway through their life, which is the rule the manager already follows. Derived from
        // the blob's own expiry rather than a configured TTL: the TTL is the master's setting, and
        // a copy of it here would only be a second place for the two to disagree.
        //
        // A blob with no readable expiry falls back to asking again on the next tick. Noisy, and
        // the alternative is an instance whose token quietly expires - which looks like the
        // application being broken rather than its credentials being stale.
        const auto expiresAt = textField(response.body, "expiresAt");
        if (expiresAt.empty()) {
            log_warning << "Credentials for instance " << assignment.instanceId << " carry no expiry; will ask again next tick";
            return issuedAt;
        }

        try {
            const auto expiry = Core::DateTimeUtils::FromISO8601(expiresAt);
            if (expiry <= issuedAt) {
                log_warning << "Credentials for instance " << assignment.instanceId << " are already expired";
                return issuedAt;
            }
            return issuedAt + (expiry - issuedAt) / 2;
        } catch (const std::exception &e) {
            log_warning << "Could not read the credentials expiry for instance " << assignment.instanceId
                        << ", error: " << e.what();
            return issuedAt;
        }
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

        // The same freshness rule the manager uses, and the reason this is cheap: a worker that
        // already ran this build downloads nothing. Compared by content rather than size, because
        // a rebuild landing on the same byte count would otherwise go on running the old build.
        //
        // The md5 is not known here - the master does not send it - so what this can check is
        // whether a file is there at all. A revision change brings the worker through stop/start,
        // and the download then overwrites. Less precise than the manager's check and in the safe
        // direction: it re-fetches where the manager would have skipped.
        if (!std::filesystem::exists(target, ec)) {
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

        const auto credentialsRefreshAt = writeCredentials(assignment);
        if (!credentialsRefreshAt.has_value()) return false;

#ifdef _WIN32
        // Deliberately not implemented. The design has nothing POSIX-specific in it, but spawning
        // and reaping a supervised child is two implementations and the manager's Windows one took
        // real care to get right - see Controller::spawnInstance. worker-nodes.md §11 puts Windows
        // workers out of scope for a first version, and a half-done one that leaks processes would
        // be worse than none.
        log_error << "euclid-wrk does not run applications on Windows - see docs/worker-nodes.md §11";
        return false;
#else
        const auto executable = assignment.runtime == "BINARY"
                                        ? target.string()
                                        : Core::Configuration::instance().getOr<std::string>("euclid.worker.java", "java");

        std::vector<std::string> arguments;
        if (assignment.runtime != "BINARY") {
            arguments.emplace_back("-jar");
            arguments.push_back(target.string());
        }
        for (const auto &argument: assignment.arguments) arguments.push_back(argument);

        const auto pid = fork();
        if (pid < 0) {
            log_error << "Could not fork for instance " << assignment.instanceId << ": " << strerror(errno);
            return false;
        }

        if (pid == 0) {
            // The child. Its own process group, so stopping it does not depend on the worker's own
            // signal disposition and a kill reaches whatever it started.
            setpgid(0, 0);

            std::vector<char *> argv;
            argv.push_back(const_cast<char *>(executable.c_str()));
            for (auto &argument: arguments) argv.push_back(const_cast<char *>(argument.c_str()));
            argv.push_back(nullptr);

            const auto credentialsPath = (directory / "credentials.json").string();
            setenv("EUCLID_CREDENTIALS_FILE", credentialsPath.c_str(), 1);
            setenv("EUCLID_INSTANCE_ID", assignment.instanceId.c_str(), 1);
            setenv("EUCLID_APPLICATION_ID", assignment.applicationId.c_str(), 1);

            execvp(executable.c_str(), argv.data());

            // Only reachable when exec failed. _exit rather than exit: this is a forked child of a
            // process holding log sinks and sockets, and running their destructors here would
            // flush and close things the parent still owns.
            _exit(127);
        }

        _instances[assignment.instanceId] = Instance{.assignment = assignment,
                                                     .pid = static_cast<int>(pid),
                                                     .httpPort = 0,
                                                     .credentialsRefreshAt = *credentialsRefreshAt};
        log_info << "Started instance " << assignment.instanceId << ", application: " << assignment.applicationId
                 << ", pid: " << pid;

        report(assignment, static_cast<int>(pid), 0, "RUNNING");
        return true;
#endif
    }

    void WorkerClient::stop(const Reconciler::Running &running) {

        const auto found = _instances.find(running.instanceId);
        if (found == _instances.end()) return;

#ifndef _WIN32
        if (const auto pid = found->second.pid; pid > 0) {
            // The group, not the process: a JVM that started helpers would otherwise leave them
            // behind, and they would go on consuming the queue this instance was reading.
            kill(-pid, SIGTERM);

            for (int waited = 0; waited < 100; ++waited) {
                if (waitpid(pid, nullptr, WNOHANG) != 0) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (waitpid(pid, nullptr, WNOHANG) == 0) {
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
                                                              {"host", Core::SystemUtils::GetHostName()},
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
        for (const auto &running: plan.stop) stop(running);
        for (const auto &assignment: plan.start) std::ignore = start(assignment);
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
        for (const auto &running: Running()) stop(running);
    }

}// namespace Euclid::Worker
