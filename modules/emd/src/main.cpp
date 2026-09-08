// C++ includes
#include <filesystem>
#include <iostream>

// Boost includes
#include <boost/program_options.hpp>

// Euclid includes
#include <EmdServer.h>
#include <euclid/core/Configuration.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/Version.h>

#define DEFAULT_LOG_LEVEL          "info"
#ifdef _WIN32
#define DEFAULT_CONFIGURATION_FILE "C:\\Program Files\\euclid\\etc\\euclid.json"
#define DEFAULT_SOCKET_PATH        "C:\\Program Files\\euclid\\data\\run\\euclid-emd.sock"
#else
#define DEFAULT_CONFIGURATION_FILE "/usr/local/euclid/etc/euclid.json"
#define DEFAULT_SOCKET_PATH        "/var/run/euclid/euclid-emd.sock"
#endif

namespace po = boost::program_options;

namespace {
    struct CliOptions {
        std::string socketPath;
        std::string configFile;
        std::string logLevel;
        bool consoleLog{true};
        bool fileLog{false};
    };
}// namespace

static std::optional<CliOptions> parseCommandLine(int argc, char *argv[]) {

    CliOptions opts;

    po::options_description general("General options", 120, 50);
    general.add_options()
            ("help,h", "Show this help message")
            ("version,v", "Show version information")
            ("config,c", po::value<std::string>(&opts.configFile)->default_value(DEFAULT_CONFIGURATION_FILE), "Path to JSON configuration file")
            ("socket,s", po::value<std::string>(&opts.socketPath)->default_value(DEFAULT_SOCKET_PATH), "Unix domain socket path (ignored: see below)");

    po::options_description logging("Logging options", 120, 50);
    logging.add_options()
            ("loglevel,l", po::value<std::string>(&opts.logLevel)->default_value(DEFAULT_LOG_LEVEL), "Log level (trace|debug|info|warning|error|fatal)")
            ("console-log", po::value<bool>(&opts.consoleLog)->default_value(true)->implicit_value(true), "Enable console logging")
            ("file-log", po::value<bool>(&opts.fileLog)->default_value(false)->implicit_value(true), "Enable file logging");

    po::options_description all("EMD options");
    all.add(general).add(logging);

    try {
        po::variables_map vm;
        po::store(po::command_line_parser(argc, argv).options(all).run(), vm);

        if (vm.contains("help")) {
            std::cout << "EMD v" << APP_VERSION << " - euclid memory database\n\n"
                      << "Holds one in-memory document store and serves it to every module over a Unix\n"
                      << "socket, so an installation can run with no MongoDB beside it. Off by default.\n\n"
                      << "Unlike every other module it listens on the socket named in the configuration\n"
                      << "rather than the one the manager passes: the modules have to know where to find\n"
                      << "it before it exists, so the address cannot depend on a process id.\n\n"
                      << all << "\n";
            return std::nullopt;
        }

        if (vm.contains("version")) {
            std::cout << "EMD version " << APP_VERSION << "\n";
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
    Euclid::Core::LogStream::SetProcessChannel("emd");
    Euclid::Core::LogStream::ApplyConfiguration(cliOpts->logLevel);

    // The configured path rather than the instance socket the manager hands every other module.
    // Those carry a process id, and a module that needs the store cannot be told the id of a
    // process that may not have started yet - so this one address is fixed and everything dials
    // it. The manager is told to judge readiness by the process staying alive instead of by a
    // socket appearing where it expected one; see euclid.modules.emd.readiness.
    const auto socketPath = cfg.getOr<std::string>("euclid.modules.emd.socketPath", cliOpts->socketPath);

    // No database initialization of any kind: this process *is* the database, and reaching for
    // another one would be a loop.
    try {
        Euclid::EMD::EmdServer server(socketPath, static_cast<int>(cfg.getOr<long>("euclid.modules.emd.threads", 8)));
        log_info << "Memory database listening, socket: " << socketPath;
        return server.RunUntilSignal();
    } catch (const std::exception &e) {
        log_error << "Failed to start EMD service: " << e.what();
        return 1;
    } catch (...) {
        log_error << "Failed to start EMD service: unknown exception type";
        return 1;
    }
}
