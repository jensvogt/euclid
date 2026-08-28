// C++ includes
#include <chrono>
#include <fstream>
#include <iterator>
#include <sstream>

// Euclid includes
#include <euclid/cli/emm/EmmCli.h>
#include <euclid/core/DateTimeUtils.h>

namespace Euclid::CLI {

    namespace po = boost::program_options;

    namespace {
        // Splits a comma-separated "--module" value into trimmed, non-empty module names, e.g.
        // "esm, eqs ,ens" -> ["esm", "eqs", "ens"].
        std::vector<std::string> SplitModules(const std::string &value) {
            std::vector<std::string> modules;
            std::stringstream ss(value);
            for (std::string part; std::getline(ss, part, ','); ) {
                const auto first = part.find_first_not_of(" \t");
                const auto last = part.find_last_not_of(" \t");
                if (first == std::string::npos) continue;
                modules.push_back(part.substr(first, last - first + 1));
            }
            return modules;
        }

        std::string JoinModules(const std::vector<std::string> &modules) {
            std::string joined;
            for (const auto &module: modules) {
                if (!joined.empty()) joined += '-';
                joined += module;
            }
            return joined;
        }
    }// namespace

    EmmCli::EmmCli(std::string endpoint, Credentials::Entry authentication, const bool pretty, std::string caCertPath) : _endpoint(std::move(endpoint)), _authentication(std::move(authentication)), _pretty(pretty), _caCertPath(std::move(caCertPath)) {}

    int EmmCli::process(const std::string &action, const std::vector<std::string> &args) const {
        if (action == "help" || action == "--help" || action == "-h") {
            return PrintModuleHelp("emm", {
                                           {"list-modules", "List modules known to the manager"},
                                           {"export", "Exports a module's MongoDB collections to a JSON file"},
                                           {"import", "Imports a JSON file written by \"emm export\" back into MongoDB"},
                                   });
        }

        // Every emm action is administrator-only. Checked here (once, ahead of dispatch) rather
        // than per-action, so a new action added later doesn't need to remember this itself. This
        // is a client-side fail-fast for a better error message only, not the real security
        // boundary - the server independently re-checks admin privileges on every request and
        // would reject it regardless of what a modified/older CLI binary sends.
        if (_authentication.token.empty()) {
            std::cerr << "error: " << action << " failed: not authenticated; run 'euclid-cli eam login' again\n";
            return 1;
        }
        if (!_authentication.isAdmin) {
            std::cerr << "error: " << action << " failed: administrator privileges required\n";
            return 1;
        }

        if (action == "list-modules") {
            return listModules(args);
        }
        if (action == "export") {
            return exportModule(args);
        }
        if (action == "import") {
            return importModule(args);
        }
        std::cerr << "error: unknown emm action '" << action << "'\n";
        return 1;
    }

    int EmmCli::listModules(const std::vector<std::string> &args) const {
        const po::options_description desc("list modules options");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("emm", "list-modules", "",
                                   "Lists modules known to the manager, as persisted in the module repository.",
                                   desc);
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("emm", "list-modules", boost::json::object{});
            if (!response.IsSuccess()) {
                std::cerr << "error: list-modules failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EmmCli::exportModule(const std::vector<std::string> &args) const {
        po::options_description desc("export options");
        desc.add_options()
                ("module,m", po::value<std::string>(), "comma-separated modules to export (eam, emm, emo, ens, eqs, esm), e.g. esm,eqs,ens - mutually exclusive with --all")
                ("all", po::bool_switch()->default_value(false), "export every module - mutually exclusive with --module")
                ("full", po::bool_switch()->default_value(false), "also export bulk child data (EQS/ENS messages, ESM objects), not just the top-level resources")
                ("file,f", po::value<std::string>(), "output file path; defaults to <modules>-export-<unix-timestamp>.json in the current directory");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("emm", "export", "(--module <module>[,<module>...] | --all) [--full] [--file <path>]",
                                   "Exports one or more modules' own MongoDB collections to a local JSON file. "
                                   "--module takes a comma-separated list (e.g. esm,eqs,ens); --all exports every "
                                   "module instead; exactly one of the two is required. By default only the "
                                   "top-level resource collection(s) are included - the same resources euclid-cli's "
                                   "own list-* actions show (queues, topics, buckets, users, ...); --full additionally "
                                   "includes the bulk child data each resource owns (EQS/ENS messages, ESM objects).",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n" << desc << std::endl;
            return 1;
        }

        const auto all = vm["all"].as<bool>();
        const auto hasModule = vm.contains("module") && !vm["module"].as<std::string>().empty();
        if (all == hasModule) {
            std::cerr << "error: exactly one of --module or --all is required\n\n" << desc << std::endl;
            return 1;
        }
        const auto modules = hasModule ? SplitModules(vm["module"].as<std::string>()) : std::vector<std::string>{};
        if (hasModule && modules.empty()) {
            std::cerr << "error: --module requires at least one module name\n\n" << desc << std::endl;
            return 1;
        }

        const auto full = vm["full"].as<bool>();
        const auto filePath = vm.contains("file") ? vm["file"].as<std::string>()
                                                   : (all ? std::string("all") : JoinModules(modules)) + "-export-" +
                                                             std::to_string(Core::DateTimeUtils::UnixTimestamp(std::chrono::system_clock::now())) + ".json";

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            boost::json::object request{{"full", full}};
            if (all) {
                request["all"] = true;
            } else {
                request["modules"] = boost::json::array(modules.begin(), modules.end());
            }
            const HttpResponse response = client.Post("emm", "export", request);
            if (!response.IsSuccess()) {
                std::cerr << "error: export failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }

            std::ofstream out(filePath, std::ios::trunc);
            if (!out.is_open()) {
                std::cerr << "error: could not open file '" << filePath << "' for writing\n";
                return 1;
            }
            out << boost::json::serialize(response.body);
            out.close();

            Core::WriteJson(std::cout, boost::json::value{{"modules", response.body.at("modules")}, {"full", full}, {"file", filePath}}, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EmmCli::importModule(const std::vector<std::string> &args) const {
        po::options_description desc("import options");
        desc.add_options()
                ("file,f", po::value<std::string>()->required(), "JSON file previously written by \"emm export\"")
                ("module,m", po::value<std::string>(), "comma-separated subset of the file's modules to import, e.g. esm,eqs; defaults to everything the file contains");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("emm", "import", "--file <path> [--module <module>[,<module>...]]",
                                   "Imports a JSON file previously written by \"emm export\" back into MongoDB, "
                                   "upserting each document by its \"_id\" (replacing the whole document, not "
                                   "merging fields, so the result matches the export exactly). --module optionally "
                                   "restricts the import to a comma-separated subset of the modules present in the "
                                   "file; by default everything the file contains is imported.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n" << desc << std::endl;
            return 1;
        }

        const auto filePath = vm["file"].as<std::string>();
        std::ifstream in(filePath, std::ios::binary);
        if (!in.is_open()) {
            std::cerr << "error: could not open file '" << filePath << "' for reading\n";
            return 1;
        }
        const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();

        boost::json::value fileJson;
        try {
            fileJson = boost::json::parse(content);
        } catch (const std::exception &ex) {
            std::cerr << "error: '" << filePath << "' is not valid JSON: " << ex.what() << std::endl;
            return 1;
        }
        if (!fileJson.is_object() || !fileJson.as_object().if_contains("collections")) {
            std::cerr << "error: '" << filePath << "' has no \"collections\" object - not an \"emm export\" file?\n";
            return 1;
        }

        if (vm.contains("module")) {
            const auto modules = SplitModules(vm["module"].as<std::string>());
            if (modules.empty()) {
                std::cerr << "error: --module requires at least one module name\n\n" << desc << std::endl;
                return 1;
            }
            fileJson.as_object()["modules"] = boost::json::array(modules.begin(), modules.end());
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("emm", "import", fileJson);
            if (!response.IsSuccess()) {
                std::cerr << "error: import failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

}