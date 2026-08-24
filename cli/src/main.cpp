// C++ includes
#include <iostream>
#include <string>
#include <vector>

// Boost includes
#include <boost/program_options.hpp>

// Euclid includes
#include <euclid/cli/credentials/Credentials.h>
#include <euclid/cli/eam/EamCli.h>
#include <euclid/cli/eqs/EqsCli.h>
#include <euclid/cli/esm/EsmCli.h>
#include <euclid/core/Version.h>

#define DEFAULT_ENDPOINT "https://localhost:5566"
#ifdef _WIN32
#define DEFAULT_CERT "C:\\Program Files\\euclid\\etc\\euclid_cert.crt"
#else
#define DEFAULT_CERT "/usr/local/euclid/etc/euclid_cert.crt"
#endif

namespace po = boost::program_options;

int main(const int argc, char *argv[]) {
    po::options_description desc("euclid-cli options", 160);
    desc.add_options()
            ("help,h", "print this help message and exit")
            ("version,v", "print version information and exit")
            ("pretty,p", po::value<bool>()->default_value(true), "pretty print output")
            ("endpoint,e", po::value<std::string>()->default_value(DEFAULT_ENDPOINT), "service endpoint URL")
            ("ca-cert", po::value<std::string>()->default_value(DEFAULT_CERT), "path to a PEM CA certificate to trust in addition to the system trust store (e.g. for self-signed development certificates)")
            ("loglevel,l", po::value<std::string>()->default_value("info"), "log level (trace|debug|info|warning|error|fatal)");

    po::options_description hidden("Hidden options");
    hidden.add_options()
            ("module", po::value<std::string>(), "module name, e.g. queues, sns, s3")
            ("action", po::value<std::string>(), "action to perform, e.g. list-queues")
            ("args", po::value<std::vector<std::string> >(), "additional arguments for the action");

    po::options_description all;
    all.add(desc).add(hidden);

    po::positional_options_description pos;
    pos.add("module", 1).add("action", 1).add("args", -1);

    const std::string usage = "Usage: euclid-cli [options] <module> <action> [args...]\n"
            "Example: euclid-cli --endpoint http://localhost:5566 queues list-queues\n";

    po::variables_map vm;
    std::vector<std::string> args;
    try {
        // allow_unregistered() lets anything after <module> <action> (including further
        // "--flag value" style options meant for the action) pass through untouched.
        const po::parsed_options parsed = po::command_line_parser(argc, argv).options(all).positional(pos).allow_unregistered().run();
        po::store(parsed, vm);
        po::notify(vm);

        args = po::collect_unrecognized(parsed.options, po::include_positional);
        // The first two collected tokens are the module and action themselves; drop them.
        if (args.size() >= 2) {
            args.erase(args.begin(), args.begin() + 2);
        } else {
            args.clear();
        }
    } catch (const po::error &ex) {
        std::cerr << "error: " << ex.what() << "\n\n" << usage << "\n" << desc << std::endl;
        return 1;
    }

    if (vm.contains("help")) {
        std::cout << usage << "\n" << desc << std::endl;
        return 0;
    }

    if (vm.contains("version")) {
        std::cout << "euclid-cli version " << APP_VERSION << std::endl;
        return 0;
    }

    if (!vm.contains("module") || !vm.contains("action")) {
        std::cerr << "error: <module> and <action> are required\n\n" << usage << "\n" << desc << std::endl;
        return 1;
    }

    const std::string endpoint = vm.contains("endpoint") ? vm["endpoint"].as<std::string>() : std::string();
    const std::string caCert = vm.contains("ca-cert") ? vm["ca-cert"].as<std::string>() : std::string();
    const std::string module = vm["module"].as<std::string>();
    const std::string action = vm["action"].as<std::string>();

    if (module == "eam") {
        const auto authToken = Euclid::CLI::Credentials::Load();
        const Euclid::CLI::EamCli eam(endpoint, authToken.value_or(Euclid::CLI::Credentials::Entry{}), true, caCert);
        return eam.process(action, args);
    }
    if (module == "eqs") {
        const auto authToken = Euclid::CLI::Credentials::Load();
        const Euclid::CLI::EqsCli eqs(endpoint, authToken.value_or(Euclid::CLI::Credentials::Entry{}), true, caCert);
        return eqs.process(action, args);
    }
    if (module == "esm") {
        const auto authToken = Euclid::CLI::Credentials::Load();
        const Euclid::CLI::EsmCli esm(endpoint, authToken.value_or(Euclid::CLI::Credentials::Entry{}), true, caCert);
        return esm.process(action, args);
    }

    std::cerr << "error: unknown module '" << module << "'\n\n" << usage << std::endl;
    return 1;
}