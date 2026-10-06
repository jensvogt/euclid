// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// euclid-wrk: runs applications euclid placed on this host.
//
// A euclid client, not a module. It reaches the gateway over signed HTTP, holds a role, and has no
// MongoDB credentials, no EMD, no Unix sockets anyone connects to, and nothing listening that the
// internet can reach. See docs/worker-nodes.md.
//
// It decides nothing. How many instances an application runs, and which node the next one goes on,
// are the master's - this reconciles toward what it is told, reports what it did, and stops its own
// work when its lease runs out.
//

// C++ includes
#include <atomic>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <optional>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#endif

// Boost includes
#include <boost/program_options.hpp>

// Euclid includes
#include <WorkerClient.h>
#include <WorkerReconciler.h>
#include <euclid/cli/credentials/Credentials.h>
#include <euclid/core/Configuration.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/SystemUtils.h>
#if defined(_WIN32)
#include <WorkerService.h>
#endif

namespace po = boost::program_options;

namespace {

    std::atomic_bool g_shutdownRequested{false};

    void requestShutdown() { g_shutdownRequested = true; }

#if !defined(_WIN32)
    void onSignal(int) { requestShutdown(); }
#endif

    std::map<std::string, std::string> parseLabels(const std::vector<std::string> &values) {

        std::map<std::string, std::string> labels;
        for (const auto &value: values) {
            if (const auto equals = value.find('='); equals != std::string::npos) {
                labels.emplace(value.substr(0, equals), value.substr(equals + 1));
            } else {
                // A bare label is a label whose presence is the point - "gpu" rather than
                // "gpu=true" - and is kept as such rather than refused.
                labels.emplace(value, "true");
            }
        }
        return labels;
    }

    // Where artifacts, credentials and logs go when nothing says otherwise. Platform-specific
    // because the POSIX default is a path that does not exist on Windows and cannot be created
    // there - a worker started by hand with no --data-dir would fail on its first fetch, somewhere
    // well away from the thing that was actually wrong. The MSI passes --data-dir explicitly, the
    // same way it passes --config, so the service does not depend on this.
    const char *defaultDataDir() {
#if defined(_WIN32)
        return R"(C:\Program Files\euclid-wrk\data)";
#else
        return "/var/lib/euclid-wrk";
#endif
    }

    // Everything main() parsed, in one place so the Windows service entry point can reach it.
    // serviceMain's own argc/argv are for extra start parameters (see StartService's
    // lpServiceArgVectors) and are normally empty, because the real command line is baked into the
    // service's binPath and delivered to this process's ordinary main(argc, argv).
    struct CliOptions {
        std::string configFile;

        // Which credentials file to read, when it must not be the invoking user's own.
        //
        // A worker is a euclid client and is logged in the way any other is (§3.2), which means its
        // token comes from the file "euclid-cli eam login" writes - $HOME/.euclid/credentials. That
        // works for a worker started by a person and does not work for a Windows service: a service
        // runs as Local System, whose USERPROFILE is C:\Windows\system32\config\systemprofile, so
        // the file an administrator just wrote at their own prompt is one the service never reads.
        //
        // So the path can be stated. The operator logs in as this node's principal and puts the
        // file where the service is told to look; nothing about the identity, the role or the audit
        // trail changes, only which directory it is read from. Empty means the ordinary
        // per-user location, which is what a worker run by hand should use.
        std::string credentialsFile;

        // Wait for the configuration file, and then the credentials it names, to exist rather than
        // fail without them - the local node's case, whose files the manager writes when it starts.
        bool waitForConfig{};

        Euclid::Worker::Options worker;

#if defined(_WIN32)
        bool install{};
        bool uninstall{};

        // Skips the Windows Service dispatch (StartServiceCtrlDispatcher) and runs the same loop as
        // an ordinary console process instead - for interactive and development use. Without this,
        // running the exe directly rather than through the SCM just fails
        // StartServiceCtrlDispatcher with ERROR_FAILED_SERVICE_CONTROLLER_CONNECT.
        bool foreground{};
#endif
    };

}// namespace

// ── Windows Service dispatch ──────────────────────────────
#if defined(_WIN32)
static const CliOptions *g_serviceCliOpts = nullptr;
static SERVICE_STATUS_HANDLE g_serviceStatusHandle = nullptr;
static SERVICE_STATUS g_serviceStatus{};
static DWORD g_serviceCheckpoint = 0;

static void updateServiceStatus(const DWORD state, const DWORD exitCode = NO_ERROR, const DWORD waitHint = 0, const DWORD specificExitCode = 0) {
    if (!g_serviceStatusHandle) return;
    g_serviceStatus.dwCurrentState = state;
    g_serviceStatus.dwWin32ExitCode = exitCode;
    // Only read by the SCM when dwWin32ExitCode says to look here, and it has to be zero otherwise
    // - a leftover value in this field makes a clean stop look like a failure.
    g_serviceStatus.dwServiceSpecificExitCode = (exitCode == ERROR_SERVICE_SPECIFIC_ERROR) ? specificExitCode : 0;
    g_serviceStatus.dwWaitHint = waitHint;
    g_serviceStatus.dwCheckPoint = (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) ? ++g_serviceCheckpoint : 0;
    SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
}

static DWORD WINAPI serviceCtrlHandlerEx(const DWORD ctrl, DWORD, LPVOID, LPVOID) {
    switch (ctrl) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            // Thirty seconds, because a worker asked to stop does not just exit: it stops the
            // instances it is running and reports them, so the master knows they are gone rather
            // than having to wait out a lease. See the tail of RunWorker.
            updateServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 30000);
            requestShutdown();
            return NO_ERROR;
        case SERVICE_CONTROL_INTERROGATE:
            return NO_ERROR;
        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

// Windows has no SIGTERM delivery across processes, so a console control handler is the worker's
// equivalent of the signal handlers below - Ctrl+C, Ctrl+Break, the console window closing, or a
// system shutdown all funnel through here. Only used in --foreground; under the SCM it is
// serviceCtrlHandlerEx that gets told.
static BOOL WINAPI consoleHandler(const DWORD ctrlType) {
    switch (ctrlType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            requestShutdown();
            return TRUE;
        default:
            return FALSE;
    }
}
#endif

// ── The worker ────────────────────────────────────────────
// reportServiceStatus is false for an ordinary console run and true under the SCM, which is the
// only thing that changes: the loop, the lease and the stop are identical either way. A service
// that took a different path through its own startup than the one a developer runs is a service
// whose failures are only reproducible in production.
// Waits until a file exists, for as long as nobody asks this process to stop. Said once, not once a
// second: the wait is the expected state for as long as the manager has not started yet.
static bool waitFor(const std::filesystem::path &path, const std::string &what) {

    std::error_code ec;
    if (std::filesystem::exists(path, ec)) return true;

    std::cerr << "waiting for " << what << " at " << path.string() << std::endl;
    while (!g_shutdownRequested) {
        if (std::filesystem::exists(path, ec)) return true;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return false;
}

static int RunWorker(const CliOptions &options, const bool reportServiceStatus) {

    // The local node's case: its configuration and credentials are written by the manager on the
    // same host when EAP starts (docs/worker-nodes.md §13.3), which on a boot or a first install is
    // after this process does. Without waiting it would fail, be restarted, and fail again until
    // they appear - burying the one line that says why under a restart loop's worth of them.
    if (options.waitForConfig && !options.configFile.empty() && !waitFor(options.configFile, "the configuration")) return 0;

    if (!options.configFile.empty()) {
        try {
            Euclid::Core::Configuration::instance().load(options.configFile);
        } catch (const std::exception &e) {
            std::cerr << "error: could not load the configuration: " << e.what() << std::endl;
            return 1;
        }
    }

    // A worker must not hold the installation's signing secret, and this is what makes that a
    // guarantee rather than a convention - docs/worker-nodes.md §10 step 6: "It is not optional,
    // and a worker must refuse to start without it."
    //
    // Anything holding euclid.modules.eam.jwt-secret can mint a bearer token for any principal in
    // the installation - including admin - offline, with no call to EAM and nothing in any audit
    // trail. A worker runs on a host the control plane does not own, and often one an operator
    // trusts less than the manager's. Distributing the secret there would make every worker host a
    // full compromise of the installation, and no amount of care elsewhere would get that back.
    //
    // Refused on the key's *presence*, whatever its value, because the realistic way it arrives is
    // somebody copying the manager's configuration file onto a worker host - and that file holds
    // far more than this. The check is cheap and the failure it prevents is unrecoverable.
    //
    // There is nothing to configure instead: a worker asks the master for each instance's
    // credentials through eap:issue-instance-credentials, and the master mints them.
    if (Euclid::Core::Configuration::instance().has("euclid.modules.eam.jwt-secret")) {
        std::cerr << "error: this configuration contains euclid.modules.eam.jwt-secret.\n"
                  << "       A worker must never hold the installation's signing secret: anything holding it\n"
                  << "       can mint a token for any principal, including admin, offline and unaudited.\n"
                  << "       Remove the key - a worker asks the master for each instance's credentials.\n"
                  << "       This usually means the manager's configuration file was copied here; it holds\n"
                  << "       more than this one secret, so check the rest of it too." << std::endl;
        return 1;
    }

    // What every other euclid process does on startup, and what this one did not: without it the
    // configuration's whole logging block was inert. euclid.logging.level was never applied, so a
    // worker configured for "info" logged at the library's own default and filled the journal with
    // debug lines; euclid.logging.dir and .prefix named a directory nothing ever wrote to - the
    // package creates /var/lib/euclid-wrk/log and chowns it, and it stayed empty; and the records
    // carried no process channel, so a worker could not be turned down through
    // euclid.logging.channels the way a module can.
    //
    // After the configuration is loaded, because every one of these reads it.
    Euclid::Core::LogStream::Initialize();

    // "wrk", matching the binary and the log prefix, so euclid.logging.channels.wrk turns this
    // process down without touching the applications it runs - those carry app.<application>.
    Euclid::Core::LogStream::SetProcessChannel("wrk");
    Euclid::Core::LogStream::ApplyConfiguration("");

    // A file as well as the console, when the configuration asks for one. The console goes to the
    // journal under systemd, which is where a worker's log is usually read; the file is for a host
    // where it is not, and for keeping more than the journal's retention.
    if (const auto &configuration = Euclid::Core::Configuration::instance();
        configuration.getOr<bool>("euclid.logging.file-active", false)) {
        Euclid::Core::LogStream::AddFile(configuration.getOr<std::string>("euclid.logging.dir", std::string(defaultDataDir()) + "/log"),
                                         configuration.getOr<std::string>("euclid.logging.prefix", "euclid-wrk"));
    }

    // The credentials this worker signs with, from the same file euclid-cli writes. A worker is a
    // euclid client and is logged in the way any other is: there is no separate worker identity
    // mechanism, which is what keeps its role, its grants and its audit trail ordinary.
    //
    // Crucially *not* the signing secret. A worker holding HttpActionServer::JwtSecret() could mint
    // a token for any principal in the installation, including admin, offline and unloggably - so
    // it asks the master for each instance's credentials instead. worker-nodes.md §3.2.
    //
    // Read from a named file when one was given, for the service case - see CliOptions.
    const auto credentialsFile = !options.credentialsFile.empty()
                                         ? options.credentialsFile
                                         : Euclid::Core::Configuration::instance().getOr<std::string>("euclid.worker.credentials", "");
    if (options.waitForConfig && !credentialsFile.empty() && !waitFor(credentialsFile, "the credentials")) return 0;

    const auto credentials = credentialsFile.empty()
                                     ? Euclid::CLI::Credentials::Load()
                                     : Euclid::CLI::Credentials::Load(credentialsFile);

    // A login's token, or an access key - which is what the local node is given rather than a login,
    // and is enough: everything a worker sends goes to EAP or ESM, and those are signed with the key.
    // Only EAM's own actions need the token, and a worker calls none of them.
    const bool canSign = credentials.has_value() && !credentials->accessKeyId.empty() && !credentials->secretAccessKey.empty();
    if (!credentials.has_value() || (credentials->token.empty() && !canSign)) {
        std::cerr << "error: not logged in - run 'euclid-cli eam login' as this node's principal first";
        if (!credentialsFile.empty()) std::cerr << "\n       and put the credentials file at " << credentialsFile;
        std::cerr << std::endl;
        return 1;
    }

    Euclid::Worker::Options workerOptions = options.worker;
    if (workerOptions.endpoint.empty()) {
        workerOptions.endpoint = Euclid::Core::Configuration::instance().getOr<std::string>("euclid.worker.endpoint", "");
    }
    if (workerOptions.endpoint.empty()) {
        std::cerr << "error: --endpoint is required (or euclid.worker.endpoint in the configuration)" << std::endl;
        return 1;
    }
    // The node's name and labels from the configuration too, under whatever the command line said:
    // a node's identity belongs in the file an upgrade leaves alone, not in a unit file or a
    // launchd job that it replaces. A label given on the command line wins over one of the same
    // key in the file.
    if (workerOptions.nodeName.empty()) {
        workerOptions.nodeName = Euclid::Core::Configuration::instance().getOr<std::string>("euclid.worker.node", "");
    }
    if (workerOptions.nodeName.empty()) workerOptions.nodeName = Euclid::Core::SystemUtils::GetHostName();
    if (Euclid::Core::Configuration::instance().has("euclid.worker.labels")) {
        try {
            for (const auto &[key, value]: Euclid::Core::Configuration::instance().getObject("euclid.worker.labels")) {
                if (const auto *text = std::get_if<std::string>(&value)) workerOptions.labels.try_emplace(key, *text);
                else if (const auto *flag = std::get_if<bool>(&value)) workerOptions.labels.try_emplace(key, *flag ? "true" : "false");
            }
        } catch (const std::exception &e) {
            std::cerr << "error: euclid.worker.labels has to be an object of key/value strings: " << e.what() << std::endl;
            return 1;
        }
    }
    // From the configuration as well as the command line, for the reason the unit file gives for the
    // endpoint: a master with a self-signed certificate is the ordinary case on a LAN, and trusting
    // it should not need an edit to a unit file that the next package upgrade overwrites.
    if (workerOptions.caCertPath.empty()) {
        workerOptions.caCertPath = Euclid::Core::Configuration::instance().getOr<std::string>("euclid.worker.ca-cert", "");
    }
    if (workerOptions.address.empty()) {
        workerOptions.address = Euclid::Core::Configuration::instance().getOr<std::string>("euclid.worker.address", "");
    }
    // From the configuration too, and only then the built-in default, because the default is the
    // Linux one: a macOS worker's account can write /usr/local/var/euclid-wrk and nothing under
    // /var/lib, and the launchd job - like the unit file - is not the place to say so, since an
    // upgrade overwrites it.
    if (workerOptions.dataDir.empty()) {
        workerOptions.dataDir = Euclid::Core::Configuration::instance().getOr<std::string>("euclid.worker.data-dir", defaultDataDir());
    }
    if (!workerOptions.caCertPath.empty() && !std::filesystem::exists(workerOptions.caCertPath)) {
        std::cerr << "error: CA certificate '" << workerOptions.caCertPath << "' does not exist" << std::endl;
        return 1;
    }

    Euclid::Worker::WorkerClient worker(workerOptions, *credentials);

    if (!worker.Register().has_value()) {
        std::cerr << "error: could not register with the master" << std::endl;
        return 1;
    }

    log_info << "euclid-wrk running, node: " << workerOptions.nodeName
             << ", endpoint: " << workerOptions.endpoint << ", tick: " << workerOptions.tick.count() << "s";

#if defined(_WIN32)
    // After Register(), not before: the SCM takes SERVICE_RUNNING as "this started successfully",
    // and a worker that cannot reach its master has not. Reported here rather than at the top of
    // the loop so a failed registration is a failed start, which is what shows up in services.msc
    // and in the event log.
    if (reportServiceStatus) updateServiceStatus(SERVICE_RUNNING);
#else
    (void) reportServiceStatus;
#endif

    // What the worker knows between ticks. The lease deadline is the master's figure held locally
    // and compared against a local clock, and it is deliberately not re-asked when a renewal
    // fails: the deadline a worker acts on has to be one it already has, or a worker that cannot
    // reach the master could never learn that it must stop.
    Euclid::Worker::Reconciler::State state;

    while (!g_shutdownRequested) {

        state.renewed = worker.Renew(state.assigned, state.leaseExpiresAt);

        // What exited since the last tick is given up first, so the decision below sees only what
        // is alive - and holds a crashed slot off rather than restarting it on the spot.
        worker.Reap();
        state.running = worker.Running();
        state.holdOff = worker.HoldOff();

        const auto plan = Euclid::Worker::Reconciler::Decide(state, std::chrono::system_clock::now());
        worker.Apply(plan);

        // Credentials are replaced halfway through their life, on every tick, for the reason the
        // manager does the same for the applications it runs: an instance started an hour ago is
        // holding a token that is about to expire, and rewriting the file is the only thing that
        // can replace it without restarting the process.
        //
        // After Apply, so an instance started on this tick is not asked twice; and not gated on
        // the renewal having succeeded, because an expiring token is a problem whether or not the
        // master answered the last poll - and if it cannot be reached, this fails harmlessly and
        // is tried again next tick with half a lifetime still in hand.
        worker.RefreshCredentials();

        if (plan.leaseLost) {
            // Everything has been stopped. The worker stays up and keeps trying to renew rather
            // than exiting: the master will have re-placed this work, and when contact comes back
            // the next renewal says what - if anything - this node should be running now. Exiting
            // would need something outside to restart it to find that out.
            state.assigned.clear();
        }

        // Interruptible, in whole seconds: a worker asked to stop should not take a tick to
        // notice. Nothing here is sensitive to the granularity - the tick is ten seconds against
        // a forty-five second lease.
        for (auto slept = std::chrono::seconds{0}; slept < workerOptions.tick && !g_shutdownRequested; ++slept) {
            std::this_thread::sleep_for(std::chrono::seconds{1});
        }
    }

    // Asked to stop, which is not the same as having lost the lease: the master has not re-placed
    // this work, so the instances are stopped and reported rather than abandoned. An operator
    // draining a node first is what avoids the gap this leaves.
    log_info << "euclid-wrk stopping, node: " << workerOptions.nodeName;
    worker.StopAll();
    return 0;
}

#if defined(_WIN32)
static void WINAPI serviceMain(DWORD, LPSTR *) {
    g_serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_serviceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;

    g_serviceStatusHandle = RegisterServiceCtrlHandlerExA(Euclid::Worker::Service::Name, serviceCtrlHandlerEx, nullptr);
    if (!g_serviceStatusHandle) return;// nothing to report to - the SCM already logged the failure

    updateServiceStatus(SERVICE_START_PENDING, NO_ERROR, 30000);
    const int rc = RunWorker(*g_serviceCliOpts, true);

    // rc goes into dwServiceSpecificExitCode, or the event log reports every failure as "terminated
    // with the following service-specific error: The operation completed successfully" - which is
    // what an unset field formats to, and which says nothing about what went wrong. The early exits
    // worth recognising there are exactly the ones that happen before there is a log file: a
    // configuration that will not load, a jwt-secret that must not be here, no credentials, no
    // endpoint, a master that cannot be reached. All of them return 1 and write to a stderr no
    // service has.
    updateServiceStatus(SERVICE_STOPPED,
                        rc == 0 ? static_cast<DWORD>(NO_ERROR) : static_cast<DWORD>(ERROR_SERVICE_SPECIFIC_ERROR),
                        0,
                        static_cast<DWORD>(rc));
}
#endif

// ── Entry point ───────────────────────────────────────────
int main(const int argc, char **argv) {

    po::options_description options("euclid-wrk options");
    options.add_options()
            ("help,h", "show this help")
            ("endpoint,e", po::value<std::string>(), "gateway to reach euclid through, e.g. https://euclid.example:5566")
            ("node,n", po::value<std::string>(), "what this node calls itself; defaults to the host name")
            ("label,l", po::value<std::vector<std::string> >()->composing(), "a placement label, key=value; repeatable")
            ("data-dir,d", po::value<std::string>(), (std::string("where artifacts, credentials and logs go; defaults to ") + defaultDataDir()).c_str())
            ("tick,t", po::value<long>()->default_value(10), "seconds between renewals")
            ("ca-cert", po::value<std::string>()->default_value(""), "a CA certificate to trust in addition to the system store")
            ("address,a", po::value<std::string>(), "IP address to report for this node; defaults to the one the master is reached from")
            ("credentials", po::value<std::string>()->default_value(""), "credentials file to read instead of the invoking user's own")
            ("wait-for-config", "wait for the configuration file and the credentials it names to exist, rather than fail")
            ("config,c", po::value<std::string>()->default_value(""), "configuration file");

#if defined(_WIN32)
    options.add_options()
            ("install", "register this executable as the Windows service 'euclid-wrk' and exit")
            ("uninstall", "stop and remove the Windows service 'euclid-wrk' and exit")
            ("foreground", "run as an ordinary console process instead of under the Service Control Manager");
#endif

    po::variables_map vm;
    try {
        po::store(po::command_line_parser(argc, argv).options(options).run(), vm);
        po::notify(vm);
    } catch (const po::error &ex) {
        std::cerr << "error: " << ex.what() << "\n\n"
                  << options << std::endl;
        return 2;
    }

    if (vm.contains("help")) {
        std::cout << options << std::endl;
        return 0;
    }

    CliOptions cliOptions;
    cliOptions.configFile = vm["config"].as<std::string>();
    cliOptions.credentialsFile = vm["credentials"].as<std::string>();
    cliOptions.waitForConfig = vm.contains("wait-for-config");
    if (vm.contains("endpoint")) cliOptions.worker.endpoint = vm["endpoint"].as<std::string>();
    if (vm.contains("node")) cliOptions.worker.nodeName = vm["node"].as<std::string>();
    if (vm.contains("data-dir")) cliOptions.worker.dataDir = vm["data-dir"].as<std::string>();
    cliOptions.worker.tick = std::chrono::seconds{std::max(1L, vm["tick"].as<long>())};
    cliOptions.worker.caCertPath = vm["ca-cert"].as<std::string>();
    if (vm.contains("address")) cliOptions.worker.address = vm["address"].as<std::string>();
    if (vm.contains("label")) cliOptions.worker.labels = parseLabels(vm["label"].as<std::vector<std::string> >());

#if defined(_WIN32)
    cliOptions.install = vm.contains("install");
    cliOptions.uninstall = vm.contains("uninstall");
    cliOptions.foreground = vm.contains("foreground");

    if (cliOptions.install && cliOptions.uninstall) {
        std::cerr << "error: --install and --uninstall are mutually exclusive" << std::endl;
        return 2;
    }

    if (cliOptions.install) {
        if (const std::string error = Euclid::Worker::Service::Install(cliOptions.configFile, cliOptions.credentialsFile); !error.empty()) {
            std::cerr << "Failed to install the euclid-wrk service: " << error << "\n";
            return 1;
        }
        std::cout << "Service 'euclid-wrk' installed (start type: automatic, not started).\n"
                     "A worker will not start until it has credentials: log in as this node's\n"
                     "principal with 'euclid-cli eam login'";
        if (!cliOptions.credentialsFile.empty()) std::cout << " and copy the file to\n  " << cliOptions.credentialsFile;
        std::cout << ".\nThen start it with:\n"
                     "  sc start euclid-wrk\n"
                     "or from services.msc.\n";
        return 0;
    }

    if (cliOptions.uninstall) {
        if (const std::string error = Euclid::Worker::Service::Uninstall(); !error.empty()) {
            std::cerr << "Failed to remove the euclid-wrk service: " << error << "\n";
            return 1;
        }
        std::cout << "Service 'euclid-wrk' stopped and removed.\n";
        return 0;
    }

    SetConsoleCtrlHandler(consoleHandler, TRUE);

    if (!cliOptions.foreground) {
        // Populated before StartServiceCtrlDispatcherA, since serviceMain has no other way to
        // reach the options this process was actually invoked with.
        g_serviceCliOpts = &cliOptions;

        SERVICE_TABLE_ENTRYA table[] = {
                {const_cast<char *>(Euclid::Worker::Service::Name), serviceMain},
                {nullptr, nullptr}
        };
        if (!StartServiceCtrlDispatcherA(table)) {
            if (GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
                std::cerr << "euclid-wrk is not running under the Service Control Manager.\n"
                             "Use --foreground to run it as an ordinary console process, or start\n"
                             "the installed service instead: sc start euclid-wrk\n";
            } else {
                std::cerr << "StartServiceCtrlDispatcher failed (error " << GetLastError() << ")\n";
            }
            return 1;
        }
        return 0;
    }
#else
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#endif

    return RunWorker(cliOptions, false);
}
