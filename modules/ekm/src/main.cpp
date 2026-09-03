// C++ includes
#include <filesystem>
#include <iostream>

// Boost includes
#include <boost/program_options.hpp>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/Version.h>
#include <euclid/core/monitoring/MetricsPusher.h>
#include <euclid/database/RepositoryFactory.h>
#include <EkmServer.h>

#define DEFAULT_LOG_LEVEL          "info"
#ifdef _WIN32
#define DEFAULT_CONFIGURATION_FILE "C:\\Program Files\\euclid\\etc\\euclid.json"
#define DEFAULT_SOCKET_PATH        "C:\\Program Files\\euclid\\data\\run\\euclid-ekm.sock"
#else
#define DEFAULT_CONFIGURATION_FILE "/usr/local/euclid/etc/euclid.json"
#define DEFAULT_SOCKET_PATH        "/var/run/euclid-ekm.sock"
#endif

namespace po = boost::program_options;

struct CliOptions {
    std::string socketPath;
    std::string configFile;
    std::string logLevel;
    bool consoleLog{true};
    bool fileLog{false};
};

static std::optional<CliOptions> parseCommandLine(int argc, char *argv[]) {

    CliOptions opts;

    po::options_description general("General options", 120, 50);
    general.add_options()
            ("help,h", "Show this help message")
            ("version,v", "Show version information")
            ("config,c", po::value<std::string>(&opts.configFile)->default_value(DEFAULT_CONFIGURATION_FILE), "Path to JSON configuration file")
            ("socket,s", po::value<std::string>(&opts.socketPath)->default_value(DEFAULT_SOCKET_PATH), "Unix domain socket path");

    po::options_description logging("Logging options", 120, 50);
    logging.add_options()
            ("loglevel,l", po::value<std::string>(&opts.logLevel)->default_value(DEFAULT_LOG_LEVEL), "Log level (trace|debug|info|warning|error|fatal)")
            ("console-log", po::value<bool>(&opts.consoleLog)->default_value(true)->implicit_value(true), "Enable console logging")
            ("file-log", po::value<bool>(&opts.fileLog)->default_value(false)->implicit_value(true), "Enable file logging");

    po::options_description all("EKM options");
    all.add(general).add(logging);

    try {
        po::variables_map vm;
        po::store(po::command_line_parser(argc, argv).options(all).run(), vm);

        if (vm.contains("help")) {
            std::cout << "EKM v" << APP_VERSION << " - EKM service process\n\n" << all << "\n";
            return std::nullopt;
        }

        if (vm.contains("version")) {
            std::cout << "EKM version " << APP_VERSION << "\n";
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

static int initializeDatabase(const Euclid::Core::Configuration &cfg) {

    try {

        // Choose backend from config
        if (const auto backend = cfg.getOr<std::string>("euclid.database.backend", "mongodb"); backend == "memory") {
            log_info << "Using in-memory database";
            Euclid::Database::RepositoryFactory::instance().initialize(Euclid::Database::BackendType::MEMORY);
        } else {
            log_info << "Using MongoDB database";
            Euclid::Database::Database::instance().initialize();
            Euclid::Database::RepositoryFactory::instance().initialize(Euclid::Database::BackendType::MONGODB);
        }

    } catch (std::exception &e) {
        log_error << "Failed to initialize database: " << e.what();
        return 1;
    } catch (...) {
        log_error << "Failed to initialize database: unknown exception type";
        return 1;
    }
    return 0;
}

int main(const int argc, char *argv[]) {

    const auto cliOpts = parseCommandLine(argc, argv);
    if (!cliOpts) return 0;

    auto &cfg = Euclid::Core::Configuration::instance();

    if (std::filesystem::exists(cliOpts->configFile)) {
        try {
            cfg.load(std::filesystem::path(cliOpts->configFile));
        } catch (const std::exception &e) {
            std::cerr << "Failed to load config: " << e.what() << "\n";
            return 1;
        }
    }

    cfg.set<bool>("euclid.logging.console-active", cliOpts->consoleLog);
    cfg.set<bool>("euclid.logging.file-active", cliOpts->fileLog);

    Euclid::Core::LogStream::Initialize();
    Euclid::Core::LogStream::SetSeverity(cfg.getOr<std::string>("euclid.logging.level", cliOpts->logLevel));

    // ── Initialize Database ─────────────────────────────
    if (const int error = initializeDatabase(cfg); error != 0) return error;
    Euclid::Database::WireAccessKeyLookup();
    Euclid::Database::WireWorkerThreadsLookup();
    Euclid::Database::WireModuleSocketLookup();
    Euclid::Database::WireScopeLookup();
    Euclid::Database::WireGrantLookup();

    Euclid::Core::Monitoring::MetricsPusher metricsPusher("ekm");
    try {
        Euclid::EKM::EkmServer server(cliOpts->socketPath,
                                   Euclid::Core::HttpActionServer::ConfiguredWorkerThreads("ekm", 2));
        return server.RunUntilSignal();
    } catch (const std::exception &e) {
        log_error << "Failed to start EKM service: " << e.what();
        return 1;
    } catch (...) {
        log_error << "Failed to start EKM service: unknown exception type";
        return 1;
    }
}