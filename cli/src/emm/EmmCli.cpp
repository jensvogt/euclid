// C++ includes
#include <chrono>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#ifndef _WIN32
#include <termios.h>
#include <unistd.h>
#endif

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

        // Reads a line from the terminal without echoing it.
        //
        // A passphrase given on the command line is in the shell's history and in every ps listing
        // for as long as the export runs, so "--passphrase" with no value asks here instead. On a
        // terminal the echo is turned off around the read; when stdin is a pipe there is nothing to
        // turn off, which is what lets a script feed one in.
        std::string ReadSecret(const std::string &prompt) {
            std::cout << prompt << std::flush;

#ifndef _WIN32
            termios original{};
            const bool isTerminal = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &original) == 0;
            if (isTerminal) {
                termios silent = original;
                silent.c_lflag &= ~static_cast<tcflag_t>(ECHO);
                tcsetattr(STDIN_FILENO, TCSAFLUSH, &silent);
            }
#endif

            std::string secret;
            std::getline(std::cin, secret);

#ifndef _WIN32
            if (isTerminal) {
                tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
                std::cout << std::endl;// the newline the user typed was not echoed either
            }
#endif
            return secret;
        }

        // The passphrase this invocation should use, or empty for an unencrypted archive.
        //
        // "--passphrase" without a value means "ask me": program_options gives an empty string for
        // it, which is the one value a passphrase may not be. Asking twice on the way in is worth
        // the second prompt - an export sealed with a mistyped passphrase is a file nobody can
        // ever open, and there is nothing to check it against later.
        std::optional<std::string> ResolvePassphrase(const po::variables_map &vm, const bool confirm) {
            if (!vm.contains("passphrase")) return std::string();

            auto passphrase = vm["passphrase"].as<std::string>();
            if (!passphrase.empty()) return passphrase;

            passphrase = ReadSecret("Passphrase: ");
            if (passphrase.empty()) {
                std::cerr << "error: the passphrase is empty\n";
                return std::nullopt;
            }
            if (confirm && ReadSecret("Repeat passphrase: ") != passphrase) {
                std::cerr << "error: the passphrases do not match\n";
                return std::nullopt;
            }
            return passphrase;
        }
    }// namespace

    EmmCli::EmmCli(std::string endpoint, Credentials::Entry authentication, const bool pretty, std::string caCertPath) : _endpoint(std::move(endpoint)), _authentication(std::move(authentication)), _pretty(pretty), _caCertPath(std::move(caCertPath)) {}

    int EmmCli::process(const std::string &action, const std::vector<std::string> &args) const {
        if (action == "help" || action == "--help" || action == "-h") {
            return PrintModuleHelp("emm", {
                                           {"list-modules", "List modules known to the manager"},
                                           {"set-instances", "Sets a module's minimum and maximum instance count"},
                                           {"set-threads", "Sets the number of worker threads a module's processes run"},
                                           {"stop-module", "Stops a module and keeps it stopped"},
                                           {"start-module", "Lets a stopped module run again"},
                                           {"restart-module", "Restarts a module's instances, one at a time"},
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
        if (action == "set-instances") {
            return setInstances(args);
        }
        if (action == "set-threads") {
            return setThreads(args);
        }
        if (action == "stop-module") {
            return setModuleStopped(args, true);
        }
        if (action == "start-module") {
            return setModuleStopped(args, false);
        }
        if (action == "restart-module") {
            return restartModule(args);
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

    int EmmCli::setInstances(const std::vector<std::string> &args) const {
        po::options_description desc("set instances options");
        desc.add_options()
                ("module,m", po::value<std::string>()->required(), "module to set the limits for, e.g. esm")
                ("min", po::value<int>(), "minimum number of instances kept running (0 or more)")
                ("max", po::value<int>(), "maximum number of instances the autoscaler may run (1 or more)");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("emm", "set-instances", "--module <module> [--min <n>] [--max <n>]",
                                   "Sets the instance limits the autoscaler works within for one module. --min is the "
                                   "floor kept running at all times, --max the ceiling it may scale up to under load; "
                                   "either may be given on its own, and the one left out keeps whatever is in force. "
                                   "The change takes effect on the manager's next reconcile, within a few seconds: "
                                   "raising the minimum starts instances up to the new floor immediately, while "
                                   "lowering the maximum lets the pool shrink through the ordinary scale-down rather "
                                   "than stopping an instance in the middle of a request. The limits are persisted and "
                                   "applied over what euclid.json configures, so they survive a manager restart and "
                                   "stay in force until they are set again.",
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

        if (!vm.contains("min") && !vm.contains("max")) {
            std::cerr << "error: at least one of --min or --max is required\n\n" << desc << std::endl;
            return 1;
        }

        boost::json::object request{{"name", vm["module"].as<std::string>()}};
        if (vm.contains("min")) request["minInstances"] = vm["min"].as<int>();
        if (vm.contains("max")) request["maxInstances"] = vm["max"].as<int>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("emm", "set-instances", request);
            if (!response.IsSuccess()) {
                std::cerr << "error: set-instances failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EmmCli::setThreads(const std::vector<std::string> &args) const {
        po::options_description desc("set threads options");
        desc.add_options()
                ("module,m", po::value<std::string>()->required(), "module to set the thread count for, e.g. eqs")
                ("threads,t", po::value<int>()->required(), "number of worker threads each instance runs (1 to 256)");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("emm", "set-threads", "--module <module> --threads <n>",
                                   "Sets how many worker threads each of a module's processes serves requests with. "
                                   "This is the companion to \"emm set-instances\": instances are how many processes "
                                   "of a module the manager runs, threads are how many requests one of those processes "
                                   "can be in the middle of. It matters most for the modules whose busiest action "
                                   "deliberately waits - EES's receive-events and EQS's receive-messages hold a thread "
                                   "for as long as the caller asked them to wait - where too few threads means requests "
                                   "that are not waiting queue behind the ones that are. "
                                   "A thread count is fixed when a process starts, so the manager applies it by "
                                   "restarting the module's instances, one per reconcile tick, so the rest of the pool "
                                   "keeps serving while it works through them; a module running a single instance is "
                                   "briefly unavailable while that one restarts. The setting is persisted and takes "
                                   "precedence over euclid.modules.<module>.threads in euclid.json, so it survives a "
                                   "restart and does not have to be applied to every host's configuration file.",
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

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("emm", "set-threads", boost::json::object{
                                                                                    {"name", vm["module"].as<std::string>()},
                                                                                    {"threads", vm["threads"].as<int>()},
                                                                            });
            if (!response.IsSuccess()) {
                std::cerr << "error: set-threads failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EmmCli::setModuleStopped(const std::vector<std::string> &args, const bool stopped) const {
        const std::string action = stopped ? "stop-module" : "start-module";

        po::options_description desc(action + " options");
        desc.add_options()
                ("module,m", po::value<std::string>()->required(), std::string(stopped ? "module to stop" : "module to start again").append(", e.g. ftp").c_str());

        if (IsHelpRequest(args)) {
            const std::string description = stopped
                    ? "Stops a module and keeps it stopped. This records desired state rather than issuing "
                      "a one-off stop, which is the difference that matters: an instance that is merely "
                      "stopped is one the manager brings straight back, whereas a module stopped here stays "
                      "down through crashes, scale events and manager restarts until \"emm start-module\" "
                      "brings it back. Its instances are stopped on the manager's next reconcile tick, within "
                      "a few seconds, and its pool slots are kept so it can be started again as it was. "
                      "Requests routed to a stopped module fail while it is down, and a module that other "
                      "modules declare as a dependency will keep those from starting - check the dependencies "
                      "before stopping something in the middle of the graph. Only euclid's own modules can be "
                      "stopped this way: applications and transfer servers have their own desired state, held "
                      "by EAP and ETS, so use \"eap stop-application\" or \"ets stop-server\" for those. "
                      "emm cannot stop itself, since there would be nothing left to start it again."
                    : "Lets a module that was stopped with \"emm stop-module\" run again. Its instances are "
                      "started on the manager's next reconcile tick, within a few seconds, back up to the "
                      "module's minimum instance count, after which the autoscaler takes over as usual. "
                      "Starting a module that was never stopped changes nothing and is not an error.";

            return PrintActionHelp("emm", action, "--module <module>", description, desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n" << desc << std::endl;
            return 1;
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("emm", action, boost::json::object{{"name", vm["module"].as<std::string>()}});
            if (!response.IsSuccess()) {
                std::cerr << "error: " << action << " failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EmmCli::restartModule(const std::vector<std::string> &args) const {
        po::options_description desc("restart-module options");
        desc.add_options()
                ("module,m", po::value<std::string>()->required(), "module to restart, e.g. esm");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("emm", "restart-module", "--module <module>",
                                   "Restarts every instance of a module, one per reconcile tick (roughly one every "
                                   "five seconds), so the rest of the pool keeps serving while it works through them; "
                                   "a module running a single instance is briefly unavailable while that one restarts. "
                                   "Nothing about the module changes - this is for picking up a new binary after an "
                                   "upgrade, clearing whatever state a process has accumulated, or making a module read "
                                   "its configuration file again. "
                                   "Unlike \"emm stop-module\" this records no desired state, so it also works for "
                                   "applications and transfer servers: the same instances come straight back, and there "
                                   "is nothing for EAP's or ETS's own reconcile to disagree with. A module that is "
                                   "stopped has nothing to restart and is refused - use \"emm start-module\" for that. "
                                   "Asking twice restarts twice; the request is carried out once and not repeated at "
                                   "the next manager start.",
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

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("emm", "restart-module", boost::json::object{{"name", vm["module"].as<std::string>()}});
            if (!response.IsSuccess()) {
                std::cerr << "error: restart-module failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
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
                ("file,f", po::value<std::string>(), "output file path; defaults to <modules>-export-<unix-timestamp>.json in the current directory")
                ("passphrase", po::value<std::string>()->implicit_value(""), "encrypt the archive; without a value the passphrase is asked for instead of being left in the shell history");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("emm", "export", "(--module <module>[,<module>...] | --all) [--full] [--file <path>] [--passphrase [<phrase>]]",
                                   "Exports one or more modules' own MongoDB collections to a local JSON file. "
                                   "--module takes a comma-separated list (e.g. esm,eqs,ens); --all exports every "
                                   "module instead; exactly one of the two is required. By default only the "
                                   "top-level resource collection(s) are included - the same resources euclid-cli's "
                                   "own list-* actions show (queues, topics, buckets, users, ...); --full additionally "
                                   "includes the bulk child data each resource owns (EQS/ENS messages, ESM objects). "
                                   "An export holds everything those collections hold - access-key secrets among it, "
                                   "and with ekm the key material itself - and then lives wherever it was written, so "
                                   "--passphrase seals it: the contents are encrypted (AES-256-GCM under a key derived "
                                   "from the passphrase) and only which modules it holds and when it was taken stay "
                                   "readable. Exporting ekm requires one. Give --passphrase without a value and it is "
                                   "asked for, twice, rather than sitting in the shell history and in every ps listing; "
                                   "\"emm import\" asks for it the same way. There is no recovery: an archive whose "
                                   "passphrase is lost is lost with it.",
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

        const auto passphrase = ResolvePassphrase(vm, true);
        if (!passphrase.has_value()) return 1;

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);

            // A sealed archive is exported one module at a time: each response is a frame, and the
            // salt the first one came back with is handed to the rest, so every frame in the file
            // opens with the same key. Whole-installation and unencrypted exports stay one request,
            // where there is nothing to tie together.
            const bool perModule = !passphrase->empty() && !all;
            const std::vector<std::string> requests = perModule ? modules : std::vector<std::string>{""};

            boost::json::array frames;
            boost::json::array exported;
            boost::json::value envelope;
            std::string salt;

            for (const auto &module: requests) {
                boost::json::object request{{"full", full}};
                if (all) {
                    request["all"] = true;
                } else if (perModule) {
                    request["modules"] = boost::json::array{module};
                } else {
                    request["modules"] = boost::json::array(modules.begin(), modules.end());
                }
                if (!passphrase->empty()) {
                    request["passphrase"] = *passphrase;
                    if (!salt.empty()) request["salt"] = salt;
                }

                const HttpResponse response = client.Post("emm", "export", request);
                if (!response.IsSuccess()) {
                    std::cerr << "error: export failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                    return 1;
                }

                if (passphrase->empty()) {
                    envelope = response.body;
                    break;
                }

                frames.push_back(response.body.at("data"));
                // Which modules the archive holds is the server's answer, not the request's: with
                // --all the caller named none, and a file that cannot say what is in it would have
                // to be opened to find out.
                for (const auto &name: response.body.at("modules").as_array()) exported.push_back(name);
                if (salt.empty()) {
                    salt = std::string(response.body.at("salt").as_string());
                    envelope = response.body;
                }
            }

            if (!passphrase->empty()) {
                // The envelope the file carries is the first frame's, minus its payload and with
                // every module named: what is left says what the archive is and how to derive its
                // key, which is what "emm import" needs before it can ask for a passphrase.
                auto &object = envelope.as_object();
                object.erase("data");
                object["modules"] = std::move(exported);
                object["frames"] = std::move(frames);
            }

            std::ofstream out(filePath, std::ios::trunc);
            if (!out.is_open()) {
                std::cerr << "error: could not open file '" << filePath << "' for writing\n";
                return 1;
            }
            out << boost::json::serialize(envelope);
            out.close();

            boost::json::object summary{{"modules", envelope.at("modules")}, {"full", full}, {"file", filePath}};
            if (!passphrase->empty()) summary["encrypted"] = true;
            Core::WriteJson(std::cout, summary, _pretty);
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
                ("module,m", po::value<std::string>(), "comma-separated subset of the file's modules to import, e.g. esm,eqs; defaults to everything the file contains")
                ("passphrase", po::value<std::string>()->implicit_value(""), "passphrase the archive was sealed with; without a value it is asked for instead of being left in the shell history");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("emm", "import", "--file <path> [--module <module>[,<module>...]] [--passphrase [<phrase>]]",
                                   "Imports a JSON file previously written by \"emm export\" back into MongoDB, "
                                   "upserting each document by its \"_id\" (replacing the whole document, not "
                                   "merging fields, so the result matches the export exactly). --module optionally "
                                   "restricts the import to a comma-separated subset of the modules present in the "
                                   "file; by default everything the file contains is imported. "
                                   "A file sealed with \"export --passphrase\" needs the same passphrase here, and is "
                                   "asked for it when --passphrase is given without a value. A wrong passphrase and an "
                                   "altered file fail the same way, and nothing is written when they do: the archive is "
                                   "authenticated as it is opened, so a half-imported archive is not a state this can "
                                   "leave behind.",
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
        const bool sealed = fileJson.is_object() && fileJson.as_object().if_contains("frames");
        if (!fileJson.is_object() || (!sealed && !fileJson.as_object().if_contains("collections"))) {
            std::cerr << "error: '" << filePath << "' has neither a \"collections\" object nor \"frames\" - not an \"emm export\" file?\n";
            return 1;
        }

        // Asked for only when the file is sealed: a passphrase means nothing to a plain archive,
        // and prompting for one would be asking for a secret that has no use.
        if (sealed) {
            if (!vm.contains("passphrase")) {
                std::cerr << "error: '" << filePath << "' is encrypted - pass --passphrase to open it\n";
                return 1;
            }
            const auto passphrase = ResolvePassphrase(vm, false);
            if (!passphrase.has_value()) return 1;
            fileJson.as_object()["passphrase"] = *passphrase;
        } else if (vm.contains("passphrase")) {
            std::cerr << "error: '" << filePath << "' is not encrypted - remove --passphrase\n";
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