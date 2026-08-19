// C++ includes
#include <csignal>

// Boost includes
#include <boost/program_options.hpp>

// Euclid includes
#include <euclid/core/SystemUtils.h>
#include <euclid/core/Version.h>
#include <euclid/core/Configuration.h>
#include <euclid/database/Database.h>
#include <euclid/database/RepositoryFactory.h>
#include <euclid/manager/Controller.h>
#include <euclid/manager/GatewayServer.h>

// ── Constants ───────────────────────────────────────────────
#define DEFAULT_CONFIGURATION_FILE "/usr/local/euclid/etc/euclid.json"
#define DEFAULT_LOG_LEVEL          "info"
#define DEFAULT_HTTP_PORT          5566

// ── Globals ───────────────────────────────────────────────
static int g_sigFd[2];
static Euclid::main::ServiceController *g_ctrl = nullptr;

// ── Signal handlers ───────────────────────────────────────
static void handleChildExit() { if (g_ctrl) g_ctrl->onChildExit(); }
static void handleShutdown() { if (g_ctrl) g_ctrl->stopAll(); }
static void handleReload() { if (g_ctrl) g_ctrl->restartAll(); }

namespace po = boost::program_options;

static void setupSignals() {

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
}

namespace {
    // ── Command line parsing ──────────────────────────────────
    struct CliOptions {
        std::string configFile;
        std::string logLevel;
        int httpPort{};
        bool consoleLog{};
        bool fileLog{};
        std::string logDir;
    };
}

static void signalDispatchLoop() {
    char sig;
    while (read(g_sigFd[0], &sig, 1) == 1) {
        switch (sig) {
            case SIGCHLD:
                handleChildExit();
                break;
            case SIGTERM:
                handleShutdown();
                break;
            case SIGHUP:
                handleReload();
                break;
            default: ;
        }
    }
}

static std::optional<CliOptions> parseCommandLine(int argc, char *argv[]) {

    CliOptions opts;

    po::options_description general("General options", 120, 50);
    general.add_options()
            ("help,h", "Show this help message")
            ("version,v", "Show version information")
            ("config,c", po::value<std::string>(&opts.configFile)->default_value(DEFAULT_CONFIGURATION_FILE), "Path to JSON configuration file");

    po::options_description server("Server options", 120, 50);
    server.add_options()
            ("port,p", po::value<int>(&opts.httpPort)->default_value(DEFAULT_HTTP_PORT), "Gateway HTTP port")
            ("loglevel,l", po::value<std::string>(&opts.logLevel)->default_value(DEFAULT_LOG_LEVEL), "Log level (trace|debug|info|warning|error|fatal)");

    po::options_description logging("Logging options", 120, 50);
    logging.add_options()
            ("console-log", po::value<bool>(&opts.consoleLog)->default_value(true)->implicit_value(true), "Enable console logging")
            ("file-log", po::value<bool>(&opts.fileLog)->default_value(false)->implicit_value(true), "Enable file logging")
            ("log-dir", po::value<std::string>(&opts.logDir)->default_value("/var/log/euclid"), "Log file directory (requires --file-log)");

    po::options_description all("euclid options");
    all.add(general).add(server).add(logging);

    try {
        po::variables_map vm;
        po::store(po::command_line_parser(argc, argv).options(all).run(), vm);

        if (vm.contains("help")) {
            std::cout << "euclid v" << APP_VERSION << " - AWS mock server\n\n" << all << "\n";
            return std::nullopt;
        }

        if (vm.contains("version")) {
            std::cout << "euclid version " << APP_VERSION << "\n";
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
                // --socket is appended per-instance (with the instance's own pid) by
                // ServiceController::spawnInstance, not baked in here.
                .args = {"--config", Euclid::Core::Configuration::instance().filePath()},
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

// ── Entry point ───────────────────────────────────────────
int main(const int argc, char *argv[]) {

    // Parse command line
    const auto cliOpts = parseCommandLine(argc, argv);
    if (!cliOpts) {
        return 0;
    }

    // Load JSON configuration
    try {
        Euclid::Core::Configuration::instance().load(std::filesystem::path(cliOpts->configFile));
    } catch (const std::exception &e) {
        std::cerr << "Failed to load config: " << e.what() << "\n";
        return 1;
    }

    // CLI overrides win over config file ─────────────
    applyCliOverrides(*cliOpts);

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

    // Module records track this manager's own child processes, so anything left over from a
    // previous run is stale (those pids/sockets no longer exist) - start from a clean slate.
    Euclid::Database::RepositoryFactory::instance().moduleRepository()->clear();

    log_info << "Euclid starting, config: " << cliOpts->configFile;
    log_info << "Gateway port: " << cfg.getOr<int>("euclid.gateway.http.port", DEFAULT_HTTP_PORT);
    log_info << "Starting euclid-manager " << APP_VERSION << ", pid: " << Euclid::Core::SystemUtils::GetPid()
            << ", loglevel: " << Euclid::Core::Configuration::instance().get<std::string>("euclid.logging.level") << ", boost: " << BOOST_LIB_VERSION;

    Euclid::main::ServiceController ctrl;
    setupSignals();

    // Register modules
    registerModules(ctrl);

    ctrl.startAll();
    ctrl.startWatchdog();

    // Start HTTP gateway
    const auto httpPort = static_cast<unsigned short>(cfg.getOr<int>("euclid.gateway.http.port", DEFAULT_HTTP_PORT));
    Euclid::main::GatewayServer gateway(ctrl, httpPort);
    gateway.start();

    // Signal dispatch blocks here
    signalDispatchLoop();

    gateway.stop();
    ctrl.stopAll();

    return 0;
}