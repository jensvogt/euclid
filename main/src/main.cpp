// C++ includes
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <iostream>
#include <mutex>
#include <optional>
#ifndef _WIN32
#include <unistd.h>
#endif

// Boost includes
#include <boost/program_options.hpp>

// Euclid includes
#include <euclid/core/SystemUtils.h>
#include <euclid/core/Version.h>
#include <euclid/core/Configuration.h>
#include <euclid/core/monitoring/MetricsPusher.h>
#include <euclid/core/monitoring/MonitoringCollector.h>
#include <euclid/database/Database.h>
#include <euclid/database/RepositoryFactory.h>
#include <euclid/manager/Controller.h>
#include <euclid/manager/ControllerPlatform.h>
#include <euclid/manager/GatewayEventIngest.h>
#include <euclid/manager/GatewayServer.h>

// ── Constants ───────────────────────────────────────────────
#ifdef _WIN32
#define DEFAULT_CONFIGURATION_FILE "C:\\Program Files\\euclid\\etc\\euclid.json"
#else
#define DEFAULT_CONFIGURATION_FILE "/usr/local/euclid/etc/euclid.json"
#endif
#define DEFAULT_LOG_LEVEL          "info"
#define DEFAULT_HTTP_PORT          5566
#define DEFAULT_HTTP_THREADS       2

namespace {
    // ── Command line parsing ──────────────────────────────────
    struct CliOptions {
        std::string configFile;
        std::string logLevel;
        int httpPort{};
        bool consoleLog{};
        bool fileLog{};
        std::string logDir;
#if defined(_WIN32)
        bool install{};
        // Skips the Windows Service dispatch (StartServiceCtrlDispatcher) and runs the same
        // startup/shutdown logic as an ordinary console process instead - for interactive/dev
        // use. Without this, running the exe directly (not launched by the SCM) just fails
        // StartServiceCtrlDispatcher with ERROR_FAILED_SERVICE_CONTROLLER_CONNECT.
        bool foreground{};
#endif
    };
}

// ── Globals ───────────────────────────────────────────────
#if defined(_WIN32)
static std::mutex g_shutdownMutex;
static std::condition_variable g_shutdownCv;
#else
static int g_sigFd[2];
#endif
static Euclid::main::ServiceController *g_ctrl = nullptr;

// Set on shutdown so signalDispatchLoop() knows to stop blocking on the next signal and let
// main() fall through to RunManager()'s own tail, which does the actual stopping (watchdog,
// then gateway, then every managed instance - see that tail's comment for why the order
// matters). Without this flag, SIGTERM would never wake signalDispatchLoop() out of its
// blocking read()/wait() and the manager process would never terminate.
static std::atomic<bool> g_shutdownRequested{false};

// ── Signal handlers ───────────────────────────────────────
#ifndef _WIN32
static void handleChildExit() { if (g_ctrl) g_ctrl->onChildExit(); }
#endif

// Deliberately does NOT stop anything itself - see g_shutdownRequested's comment. Stopping
// instances here, before the gateway is stopped, would leave it free to keep forwarding
// traffic (and the watchdog free to keep autoscaling in response to it) for the entire
// duration of that stop - long enough, for a module like esm that both runs multiple
// instances and can have slow-draining requests, for the autoscaler to spawn an instance
// that's invisible to the stop already in progress and so never gets stopped at all.
static void handleShutdown() {
    g_shutdownRequested = true;
#if defined(_WIN32)
    g_shutdownCv.notify_all();
#endif
}

static void handleReload() { if (g_ctrl) g_ctrl->restartAll(); }

namespace po = boost::program_options;

#if defined(_WIN32)
// ── Windows Service dispatch ───────────────────────────────
// Populated by main() (pointing at its own on-stack CliOptions) before calling
// StartServiceCtrlDispatcherA, since serviceMain's own argc/argv are for extra start
// parameters (see StartService's lpServiceArgVectors) - normally none here, since the
// real command line (e.g. "--config ...") is already baked into the service's binPath
// by Platform::InstallService and delivered to this process's own main(argc, argv).
static const CliOptions *g_serviceCliOpts = nullptr;
static SERVICE_STATUS_HANDLE g_serviceStatusHandle = nullptr;
static SERVICE_STATUS g_serviceStatus{};
static DWORD g_serviceCheckpoint = 0;

static void updateServiceStatus(const DWORD state, const DWORD exitCode = NO_ERROR, const DWORD waitHint = 0) {
    if (!g_serviceStatusHandle) return;
    g_serviceStatus.dwCurrentState = state;
    g_serviceStatus.dwWin32ExitCode = exitCode;
    g_serviceStatus.dwWaitHint = waitHint;
    g_serviceStatus.dwCheckPoint = (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) ? ++g_serviceCheckpoint : 0;
    SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
}

static DWORD WINAPI serviceCtrlHandlerEx(const DWORD ctrl, DWORD, LPVOID, LPVOID) {
    switch (ctrl) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            updateServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 30000);
            handleShutdown();
            return NO_ERROR;
        case SERVICE_CONTROL_INTERROGATE:
            return NO_ERROR;
        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}
#endif

#if defined(_WIN32)
// Windows has no SIGTERM/SIGHUP delivery across processes, so a console control handler
// is the manager's own equivalent of the signal handler below - Ctrl+C, Ctrl+Break, the
// console window closing, or a system/user shutdown all funnel through here. Config
// reload (SIGHUP on POSIX) has no Windows trigger wired up yet, so it's not handled.
static BOOL WINAPI consoleHandler(const DWORD ctrlType) {
    switch (ctrlType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            handleShutdown();
            return TRUE;
        default:
            return FALSE;
    }
}
#endif

static void setupSignals() {
#if defined(_WIN32)
    SetConsoleCtrlHandler(consoleHandler, TRUE);
#else

    // g_sigFd must be a real pipe before any handler below can run - without this, both "ends"
    // default to fd 0 (stdin), so the handler's write() and signalDispatchLoop()'s read() are the
    // same fd instead of two ends of a pipe. That happened to look like it worked when stdin was
    // an interactive terminal (read() just blocks on it, same as it would block on a real empty
    // pipe), but the moment stdin isn't a TTY - e.g. under systemd's default StandardInput=null,
    // or any other non-interactive launch - read() sees immediate EOF and signalDispatchLoop()
    // returns right away, tearing down every managed instance seconds after startup.
    if (pipe(g_sigFd) != 0) {
        std::cerr << "Failed to create signal pipe: " << strerror(errno) << "\n";
        std::exit(1);
    }

    struct sigaction sa{};
    memset(&sa, 0, sizeof(sa));// safest zero-init in C++

    sa.sa_handler = [](const int sig) {
        const char byte = static_cast<char>(sig);
        std::ignore = write(g_sigFd[1], &byte, 1);// async-signal-safe
    };

    sigemptyset(&sa.sa_mask);// don't block other signals
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;// restart syscalls, ignore stop

    sigaction(SIGCHLD, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);// Ctrl-C in a foreground terminal
#endif
}

static void signalDispatchLoop() {
#if defined(_WIN32)
    // Child-exit reaping has no async trigger on Windows either (see
    // ServiceController::reapExitedWindows()) - the watchdog thread polls for it every
    // tick instead, so this loop only has shutdown left to wait for.
    std::unique_lock lock(g_shutdownMutex);
    g_shutdownCv.wait(lock, [] { return g_shutdownRequested.load(); });
#else
    char sig;
    while (!g_shutdownRequested && read(g_sigFd[0], &sig, 1) == 1) {
        switch (sig) {
            case SIGCHLD:
                handleChildExit();
                break;
            case SIGTERM:
            case SIGINT:
                handleShutdown();
                break;
            case SIGHUP:
                handleReload();
                break;
            default: ;
        }
    }
#endif
}

static std::optional<CliOptions> parseCommandLine(int argc, char *argv[]) {

    CliOptions opts;

    po::options_description general("General options", 120, 50);
    general.add_options()
            ("help,h", "Show this help message")
            ("version,v", "Show version information")
            ("config,c", po::value<std::string>(&opts.configFile)->default_value(DEFAULT_CONFIGURATION_FILE), "Path to JSON configuration file")
#if defined(_WIN32)
            ("install", po::bool_switch(&opts.install), "Register this executable as the Windows service \"euclid\" (SERVICE_AUTO_START), then exit; requires an elevated (Administrator) prompt")
            ("foreground", po::bool_switch(&opts.foreground), "Run as an ordinary console process instead of dispatching through the Service Control Manager; for interactive/dev use")
#endif
            ;

    po::options_description server("Server options", 120, 50);
    server.add_options()
            ("port,p", po::value<int>(&opts.httpPort)->default_value(DEFAULT_HTTP_PORT), "Gateway HTTP port")
            ("loglevel,l", po::value<std::string>(&opts.logLevel)->default_value(DEFAULT_LOG_LEVEL), "Log level (trace|debug|info|warning|error|fatal)");

    po::options_description logging("Logging options", 120, 50);
    logging.add_options()
            ("console-log", po::value<bool>(&opts.consoleLog)->default_value(true)->implicit_value(true), "Enable console logging")
            ("file-log", po::value<bool>(&opts.fileLog)->default_value(false)->implicit_value(true), "Enable file logging")
            ("log-dir", po::value<std::string>(&opts.logDir)->default_value("/var/log/euclid"), "Log file storage (requires --file-log)");

    po::options_description all("euclid options");
    all.add(general).add(server).add(logging);

    try {
        po::variables_map vm;
        po::store(po::command_line_parser(argc, argv).options(all).run(), vm);

        if (vm.contains("help")) {
            std::cout << "Euclid v" << APP_VERSION << " - Cloud Services\n\n" << all << "\n";
            return std::nullopt;
        }

        if (vm.contains("version")) {
            std::cout << "Euclid version " << APP_VERSION << "\n";
            return std::nullopt;
        }

        po::notify(vm);

    } catch (const po::error &e) {
        std::cerr << "Command line error: " << e.what() << "\n";
        std::cerr << "Use --help for usage information.\n";
        return std::nullopt;
    }

    return opts;
}


// ── Apply CLI overrides on top of JSON config ─────────────
static void applyCliOverrides(const CliOptions &opts) {
    auto &cfg = Euclid::Core::Configuration::instance();

    // CLI always wins over config file
    if (opts.httpPort != DEFAULT_HTTP_PORT) {
        cfg.set<int>("euclid.gateway.http.port", opts.httpPort);
    }

    if (opts.logLevel != DEFAULT_LOG_LEVEL) {
        cfg.set<std::string>("euclid.logging.level", opts.logLevel);
    }

    if (!opts.logDir.empty()) {
        cfg.set<std::string>("euclid.logging.dir", opts.logDir);
    }

    cfg.set<bool>("euclid.logging.console-active", opts.consoleLog);
    cfg.set<bool>("euclid.logging.file-active", opts.fileLog);
}

static void registerModules(Euclid::main::ServiceController &ctrl) {
    for (auto modules = Euclid::Core::Configuration::instance().getObjects("euclid.modules"); const auto &[name, props]: modules) {
        const auto active_it = props.find("active");
        const bool active = active_it != props.end() && std::get<bool>(active_it->second);
        if (!active) {
            log_info << "Module: " << name << " skipped (inactive)";
            continue;
        }

        // Only modules that have been migrated to run as a separate subprocess declare these keys.
        if (!props.contains("socketPath") || !props.contains("executable") || !props.contains("name")) {
            log_info << "Module: " << name << " skipped (not a subprocess module, missing socketPath/executable/name)";
            continue;
        }
        const std::string socketPath = std::get<std::string>(props.at("socketPath"));
        const std::string executable = std::get<std::string>(props.at("executable"));
        const std::string headerName = std::get<std::string>(props.at("name"));

        // minInstances/maxInstances are optional; modules that don't set them stay single-instance.
        auto readInt = [&props](const std::string &key, const int defaultValue) {
            const auto it = props.find(key);
            return it != props.end() ? static_cast<int>(std::get<long>(it->second)) : defaultValue;
        };
        const int minInstances = readInt("minInstances", 1);
        const int maxInstances = readInt("maxInstances", 1);

        log_info << "Module: " << name << " active=" << active << " socketPath=" << socketPath
                << " minInstances=" << minInstances << " maxInstances=" << maxInstances;
        ctrl.registerModule({
                .name = headerName,
                .executable = executable,
                .args = {"--config", Euclid::Core::Configuration::instance().filePath().string()},
                .socketPath = socketPath,
                .maxRestarts = -1,
                .autoRestart = true,
                .minInstances = minInstances,
                .maxInstances = maxInstances
        });
    }
}

static int initializeDatabase(const Euclid::Core::Configuration &cfg) {

    // Choose backend from config
    if (const auto backend = cfg.getOr<std::string>("euclid.database.backend", "mongodb"); backend == "memory") {
        log_info << "Using in-memory database";
        Euclid::Database::RepositoryFactory::instance().initialize(Euclid::Database::BackendType::MEMORY);
    } else {
        log_info << "Using MongoDB database";
        Euclid::Database::Database::instance().initialize();
        Euclid::Database::RepositoryFactory::instance().initialize(Euclid::Database::BackendType::MONGODB);
    }
    return 0;
}

// Shared by both the ordinary console path and the Windows Service path (serviceMain below) -
// the only difference between the two is who calls this and how shutdown gets triggered
// (console control handler vs. service control handler), both of which just set
// g_shutdownRequested and let signalDispatchLoop() below fall through the same way.
static int RunManager(const CliOptions &opts, [[maybe_unused]] const bool reportServiceStatus) {

    // Load JSON configuration
    try {
        Euclid::Core::Configuration::instance().load(std::filesystem::path(opts.configFile));
    } catch (const std::exception &e) {
        std::cerr << "Failed to load config: " << e.what() << "\n";
        return 1;
    }

    // CLI overrides win over config file ─────────────
    applyCliOverrides(opts);

    // Initialize logging ─────────────────────────────
    const auto &cfg = Euclid::Core::Configuration::instance();
    Euclid::Core::LogStream::Initialize();
    Euclid::Core::LogStream::SetSeverity(cfg.getOr<std::string>("euclid.logging.level", "info"));

    if (cfg.getOr<bool>("euclid.logging.file-active", false)) {
        Euclid::Core::LogStream::AddFile(cfg.getOr<std::string>("euclid.logging.dir", "/var/log/euclid"), cfg.getOr<std::string>("euclid.logging.prefix", "euclid"));
    }

    // ── Initialize Database ─────────────────────────────
    if (const int error = initializeDatabase(cfg); error != 0) return error;
    Euclid::Database::WireAccessKeyLookup();
    Euclid::Database::WireScopeLookup();
    Euclid::Database::WireGrantLookup();
    Euclid::Database::WireModuleSocketLookup();

    // Module records track this manager's own child processes, so anything left over from a
    // previous run is stale (those pids/sockets no longer exist) - start from a clean slate.
    Euclid::Database::RepositoryFactory::instance().emmRepository()->clear();

    // The manager isn't built on Core::HttpActionServer (each module process is, which is where
    // every other module's MonitoringCollector::Start() call lives), so it has to start its own
    // collector here - otherwise GatewayServer's per-request MonitoringTimer would report to a
    // bus nothing is listening on, and "gateway-service-time"/"gateway-service-count" would never
    // reach the emo module.
    Euclid::Core::Monitoring::MonitoringCollector::instance().Start();

    log_info << "Euclid starting, config: " << opts.configFile;
    log_info << "Gateway port: " << cfg.getOr<int>("euclid.gateway.http.port", DEFAULT_HTTP_PORT);
    log_info << "Starting euclid-manager " << APP_VERSION << ", pid: " << Euclid::Core::SystemUtils::GetPid()
            << ", loglevel: " << Euclid::Core::Configuration::instance().get<std::string>("euclid.logging.level") << ", boost: " << BOOST_LIB_VERSION;

    Euclid::main::ServiceController ctrl;
    g_ctrl = &ctrl;// must happen before setupSignals() installs handlers that dereference it
    setupSignals();

    // Register modules
    registerModules(ctrl);

    ctrl.startAll();
    ctrl.startWatchdog();

    // Periodically pushes this process' own collected metrics (currently just
    // "gateway-service-time"/"gateway-service-count", recorded per request in GatewayServer's
    // route()) to the emo module - same mechanism every other module uses for its own metrics,
    // see Core::Monitoring::MetricsPusher's doc comment. Kept alive for the process lifetime.
    Euclid::Core::Monitoring::MetricsPusher metricsPusher("gateway");

    // Start HTTP gateway
    const auto httpPort = static_cast<unsigned short>(cfg.getOr<int>("euclid.gateway.http.port", DEFAULT_HTTP_PORT));
    const auto httpThreads = cfg.getOr<int>("euclid.gateway.http.max-thread", DEFAULT_HTTP_THREADS);
    std::optional<Euclid::main::GatewayServer> gatewayOpt;
    try {
        gatewayOpt.emplace(ctrl, httpPort, httpThreads);
    } catch (const std::exception &e) {
        log_error << "Failed to start gateway on port " << httpPort << ": " << e.what();
        ctrl.stopWatchdog();
        ctrl.stopAll();
        return 1;
    }
    auto &gateway = *gatewayOpt;
    gateway.start();

    // Receiving end of Core::EventPusher - every module process pushes business events (e.g.
    // EKM key lifecycle) here over a fixed configured Unix domain socket, the same one-shot-push
    // idiom MetricsPusher already uses for the "emo" module, just event-driven instead of
    // scheduled. Only started if a path is actually configured, so deployments that haven't set
    // euclid.gateway.event-socket-path yet are unaffected - Core::EventPusher::Push() is
    // similarly a no-op on the sending side until a module configures the same key.
    std::optional<Euclid::main::GatewayEventIngest> eventIngestOpt;
    if (const auto eventSocketPath = cfg.getOr<std::string>("euclid.gateway.event-socket-path", ""); !eventSocketPath.empty()) {
        try {
            eventIngestOpt.emplace(eventSocketPath);
            eventIngestOpt->start();
        } catch (const std::exception &e) {
            log_error << "Failed to start gateway event ingest on " << eventSocketPath << ": " << e.what();
        }
    }

#if defined(_WIN32)
    if (reportServiceStatus) updateServiceStatus(SERVICE_RUNNING);
#endif

    // Signal dispatch blocks here
    signalDispatchLoop();

    // Order matters: the watchdog must be fully stopped (join()ed) before the gateway stops
    // accepting/forwarding traffic, and the gateway must be stopped before stopAll() starts
    // working through instances - otherwise both traffic and autoscaling can still be live
    // while stopAll() is (potentially for several seconds, sequentially, per instance)
    // mid-shutdown, and the watchdog can spawn an instance that stopAll()'s already-taken
    // snapshot will never see. See handleShutdown()'s comment for the failure mode this avoids.
    ctrl.stopWatchdog();
    if (eventIngestOpt) eventIngestOpt->stop();
    gateway.stop();
    ctrl.stopAll();

    return 0;
}

#if defined(_WIN32)
static void WINAPI serviceMain(DWORD, LPSTR *) {
    g_serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_serviceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;

    g_serviceStatusHandle = RegisterServiceCtrlHandlerExA("euclid", serviceCtrlHandlerEx, nullptr);
    if (!g_serviceStatusHandle) return;// nothing to report to - SCM already logged the failure

    updateServiceStatus(SERVICE_START_PENDING, NO_ERROR, 30000);
    const int rc = RunManager(*g_serviceCliOpts, true);
    updateServiceStatus(SERVICE_STOPPED, rc == 0 ? static_cast<DWORD>(NO_ERROR) : static_cast<DWORD>(ERROR_SERVICE_SPECIFIC_ERROR));
}
#endif

// ── Entry point ───────────────────────────────────────────
int main(const int argc, char *argv[]) {

    // Parse command line
    const auto cliOpts = parseCommandLine(argc, argv);
    if (!cliOpts) {
        return 0;
    }

#if defined(_WIN32)
    if (cliOpts->install) {
        if (const std::string error = Euclid::main::Platform::InstallService(cliOpts->configFile); !error.empty()) {
            std::cerr << "Failed to install the euclid service: " << error << "\n";
            return 1;
        }
        std::cout << "Service 'euclid' installed (start type: automatic). Start it with:\n"
                     "  sc start euclid\n"
                     "or from services.msc.\n";
        return 0;
    }

    if (!cliOpts->foreground) {
        // Populate before StartServiceCtrlDispatcherA, since serviceMain has no other way
        // to reach the options this process was actually invoked with.
        g_serviceCliOpts = &*cliOpts;

        constexpr SERVICE_TABLE_ENTRYA table[] = {
                {const_cast<char *>("euclid"), serviceMain},
                {nullptr, nullptr}
        };
        if (!StartServiceCtrlDispatcherA(table)) {
            if (GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
                std::cerr << "euclid-manager is not running under the Service Control Manager.\n"
                             "Use --foreground to run it as an ordinary console process, or start\n"
                             "the installed service instead: sc start euclid\n";
            } else {
                std::cerr << "StartServiceCtrlDispatcher failed (error " << GetLastError() << ")\n";
            }
            return 1;
        }
        return 0;
    }
#endif

    return RunManager(*cliOpts, false);
}