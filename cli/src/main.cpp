// C++ includes
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

// Boost includes
#include <boost/program_options.hpp>

// Euclid includes
#include <euclid/cli/credentials/Credentials.h>
#include <euclid/cli/eam/EamCli.h>
#include <euclid/cli/eag/EagCli.h>
#include <euclid/cli/ees/EesCli.h>
#include <euclid/cli/ekm/EkmCli.h>
#include <euclid/cli/emm/EmmCli.h>
#include <euclid/cli/ens/EnsCli.h>
#include <euclid/cli/eqs/EqsCli.h>
#include <euclid/cli/esm/EsmCli.h>
#include <euclid/cli/eap/EapCli.h>
#include <euclid/cli/ets/EtsCli.h>
#include <euclid/core/Configuration.h>
#include <euclid/core/Version.h>

#define DEFAULT_ENDPOINT "https://localhost:5566"
#ifdef _WIN32
#define DEFAULT_CERT "C:\\Program Files\\euclid\\etc\\euclid_cert.crt"
#define DEFAULT_CONFIG_FILE "C:\\Program Files\\euclid\\etc\\euclid.json"
#else
#define DEFAULT_CERT "/usr/local/euclid/etc/euclid_cert.crt"
#define DEFAULT_CONFIG_FILE "/usr/local/euclid/etc/euclid.json"
#endif

namespace po = boost::program_options;

int main(const int argc, char *argv[]) {
    po::options_description desc("euclid-cli options", 160);
    desc.add_options()
            ("help,h", "print this help message and exit")
            ("version,v", "print version information and exit")
            ("pretty,p", po::value<bool>()->default_value(true), "pretty print output")
            ("endpoint,e", po::value<std::string>()->default_value(DEFAULT_ENDPOINT), "service endpoint URL")
            ("ca-cert,t", po::value<std::string>()->default_value(DEFAULT_CERT), "path to a PEM CA certificate to trust in addition to the system trust store (e.g. for self-signed development certificates)")
            ("config,c", po::value<std::string>()->default_value(DEFAULT_CONFIG_FILE), "path to a JSON configuration file providing defaults for action options (e.g. euclid.modules.esm.part-size/concurrency for esm's upload-file/download-file); silently ignored if the file doesn't exist")
            ("signature,s", po::value<std::string>(), "signature scheme for signed service calls: rfc9421 (HTTP Message Signatures, the default) or sigv4; overrides euclid.cli.signature from the config file")
            ("loglevel,l", po::value<std::string>()->default_value("info"), "log level (trace|debug|info|warning|error|fatal)");

    const std::string usage = "Usage: euclid-cli [options] <module> <action> [args...]\n"
            "Example: euclid-cli --endpoint http://localhost:5566 eqs list-queues\n"
            "Modules:\n"
            "\tEAM Euclid access management (user, user groups, accounts, namespaces)\n"
            "\tEQS Euclid queueing system (queues, messages)\n"
            "\tESM Euclid storage module (buckets, objects)\n"
            "\tEES Euclid event service (subscribe to what other modules publish)\n"
            "\tENS Euclid notifications system (pub/sub topics, messages)\n"
            "\tEKM Euclid key management (cryptographic keys, encryption, decryption)\n"
            "\tEMM Euclid module management (start, stop, restart, auto-scaler)\n"
            "\tETS Euclid transfer server (FTP/SFTP endpoints onto ESM buckets)\n"
            "\tEAP Euclid applications (Java, Python, Node.js, Rust or C++ processes euclid runs and scales)\n"
            "\tEAG Euclid API gateway (publishes paths and proxies them to EAP application instances)\n";

    // Global options are only recognized before <module> - the first token that isn't one of
    // them (or a value for one) starts <module> <action> [args...], which is taken verbatim from
    // there on and never touched by this parser again. This used to be a single flat
    // allow_unregistered() parse of the whole command line, but that meant a short option
    // letter reused by an action (e.g. eam login's "-p" for --password) collided with a
    // same-lettered global option ("-p" for --pretty): boost::program_options has no notion of
    // "only before the first positional", so it greedily matched the global one wherever "-p"
    // appeared, silently feeding "admin" to --pretty (a bool) instead of leaving it for the
    // action to parse as its password. Splitting the command line ourselves before handing
    // anything to boost::program_options sidesteps that entirely: an action's own options are
    // simply never visible to the global parser at all.
    static const std::unordered_set<std::string> kGlobalFlags{"-h", "--help", "-v", "--version"};
    static const std::unordered_set<std::string> kGlobalValueOptions{
            "-p", "--pretty", "-e", "--endpoint", "--ca-cert", "-c", "--config", "-l", "--loglevel", "-s", "--signature"
    };

    std::vector<std::string> globalArgs;
    std::vector<std::string> rest;
    {
        int i = 1;
        for (; i < argc; ++i) {
            const std::string tok = argv[i];
            if (kGlobalFlags.contains(tok)) {
                globalArgs.push_back(tok);
                continue;
            }
            if (kGlobalValueOptions.contains(tok)) {
                globalArgs.push_back(tok);
                if (i + 1 < argc) globalArgs.emplace_back(argv[++i]);
                continue;
            }
            if (const auto eq = tok.find('='); tok.starts_with("--") && eq != std::string::npos && kGlobalValueOptions.contains(tok.substr(0, eq))) {
                globalArgs.push_back(tok);
                continue;
            }
            break;// first token that isn't a recognized global option (or its value) - <module> starts here
        }
        for (; i < argc; ++i) rest.emplace_back(argv[i]);
    }

    po::variables_map vm;
    try {
        po::store(po::command_line_parser(globalArgs).options(desc).run(), vm);
        po::notify(vm);
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

    if (rest.size() < 2) {
        std::cerr << "error: <module> and <action> are required\n\n" << usage << "\n" << desc << std::endl;
        return 1;
    }

    const std::string endpoint = vm.contains("endpoint") ? vm["endpoint"].as<std::string>() : std::string();

    // The default names the certificate a euclid server installs beside itself, which is exactly
    // what a machine with only the CLI package on it does not have - and loading a file that isn't
    // there fails the TLS handshake before it starts, so every command would answer "No such file
    // or directory" without saying which file. Falling back to the system trust store is the right
    // answer there: a client talking to a server with a publicly-signed certificate needs nothing
    // else, and one talking to a self-signed server says so with --ca-cert.
    //
    // A path somebody typed is different. That is a statement that this certificate is required,
    // so a missing one is reported rather than quietly ignored - otherwise a typo downgrades the
    // connection to whatever the system happens to trust, which is the one outcome nobody asking
    // for a specific CA wants.
    std::string caCert = vm.contains("ca-cert") ? vm["ca-cert"].as<std::string>() : std::string();
    if (!caCert.empty() && !std::filesystem::exists(caCert)) {
        if (!vm["ca-cert"].defaulted()) {
            std::cerr << "error: --ca-cert '" << caCert << "' does not exist\n";
            return 1;
        }
        caCert.clear();
    }
    const bool pretty = vm["pretty"].as<bool>();
    const std::string module = rest[0];
    const std::string action = rest[1];
    const std::vector<std::string> args(rest.begin() + 2, rest.end());

    // Optional: euclid-cli is commonly run with no config at all (a bare client talking to a
    // remote endpoint), so a missing file here isn't an error - only load if it's actually
    // present, and don't let a malformed one block the CLI from running with built-in defaults.
    if (const auto configFile = vm["config"].as<std::string>(); std::filesystem::exists(configFile)) {
        try {
            Euclid::Core::Configuration::instance().load(configFile);
        } catch (const std::exception &ex) {
            std::cerr << "warning: could not load config file '" << configFile << "': " << ex.what() << "\n";
        }
    }

    // Applied after the config file is loaded, so the command line wins over
    // euclid.cli.signature; HttpClient reads it back out when it signs a service call.
    if (vm.contains("signature")) {
        Euclid::Core::Configuration::instance().set<std::string>("euclid.cli.signature", vm["signature"].as<std::string>());
    }

    if (module == "eam") {
        const auto authToken = Euclid::CLI::Credentials::Load();
        const Euclid::CLI::EamCli eam(endpoint, authToken.value_or(Euclid::CLI::Credentials::Entry{}), pretty, caCert);
        return eam.process(action, args);
    }
    if (module == "eqs") {
        const auto authToken = Euclid::CLI::Credentials::Load();
        const Euclid::CLI::EqsCli eqs(endpoint, authToken.value_or(Euclid::CLI::Credentials::Entry{}), pretty, caCert);
        return eqs.process(action, args);
    }
    if (module == "ens") {
        const auto authToken = Euclid::CLI::Credentials::Load();
        const Euclid::CLI::EnsCli ens(endpoint, authToken.value_or(Euclid::CLI::Credentials::Entry{}), pretty, caCert);
        return ens.process(action, args);
    }
    if (module == "esm") {
        const auto authToken = Euclid::CLI::Credentials::Load();
        const Euclid::CLI::EsmCli esm(endpoint, authToken.value_or(Euclid::CLI::Credentials::Entry{}), pretty, caCert);
        return esm.process(action, args);
    }
    if (module == "emm") {
        const auto authToken = Euclid::CLI::Credentials::Load();
        const Euclid::CLI::EmmCli emm(endpoint, authToken.value_or(Euclid::CLI::Credentials::Entry{}), pretty, caCert);
        return emm.process(action, args);
    }
    if (module == "eap") {
        const auto authToken = Euclid::CLI::Credentials::Load();
        const Euclid::CLI::EapCli eap(endpoint, authToken.value_or(Euclid::CLI::Credentials::Entry{}), pretty, caCert);
        return eap.process(action, args);
    }
    if (module == "ets") {
        const auto authToken = Euclid::CLI::Credentials::Load();
        const Euclid::CLI::EtsCli ets(endpoint, authToken.value_or(Euclid::CLI::Credentials::Entry{}), pretty, caCert);
        return ets.process(action, args);
    }
    if (module == "ees") {
        const auto authToken = Euclid::CLI::Credentials::Load();
        const Euclid::CLI::EesCli ees(endpoint, authToken.value_or(Euclid::CLI::Credentials::Entry{}), pretty, caCert);
        return ees.process(action, args);
    }
    if (module == "eag") {
        const auto authToken = Euclid::CLI::Credentials::Load();
        const Euclid::CLI::EagCli eag(endpoint, authToken.value_or(Euclid::CLI::Credentials::Entry{}), pretty, caCert);
        return eag.process(action, args);
    }
    if (module == "ekm") {
        const auto authToken = Euclid::CLI::Credentials::Load();
        const Euclid::CLI::EkmCli ekm(endpoint, authToken.value_or(Euclid::CLI::Credentials::Entry{}), pretty, caCert);
        return ekm.process(action, args);
    }

    std::cerr << "error: unknown module '" << module << "'\n\n" << usage << std::endl;
    return 1;
}