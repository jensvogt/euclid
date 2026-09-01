// C++ includes
#include <sstream>

// Euclid includes
#include <euclid/cli/eap/EapCli.h>

namespace Euclid::CLI {

    namespace po = boost::program_options;

    namespace {

        // Splits "KEY=value,OTHER=value" into a JSON object. Environment variables are a map on
        // the wire but a single argument on the command line, and only the first '=' separates
        // name from value - the value may well contain more of them.
        boost::json::object SplitEnvironment(const std::string &value) {
            boost::json::object environment;
            std::stringstream ss(value);
            for (std::string part; std::getline(ss, part, ','); ) {
                const auto first = part.find_first_not_of(" \t");
                if (first == std::string::npos) continue;
                const auto equals = part.find('=', first);
                if (equals == std::string::npos) continue;
                environment[part.substr(first, equals - first)] = part.substr(equals + 1);
            }
            return environment;
        }

        boost::json::array SplitList(const std::string &value) {
            boost::json::array entries;
            std::stringstream ss(value);
            for (std::string part; std::getline(ss, part, ','); ) {
                const auto first = part.find_first_not_of(" \t");
                const auto last = part.find_last_not_of(" \t");
                if (first == std::string::npos) continue;
                entries.push_back(boost::json::string(part.substr(first, last - first + 1)));
            }
            return entries;
        }

    }// namespace

    EapCli::EapCli(std::string endpoint, Credentials::Entry authentication, const bool pretty, std::string caCertPath) : _endpoint(std::move(endpoint)), _authentication(std::move(authentication)), _pretty(pretty), _caCertPath(std::move(caCertPath)) {}

    int EapCli::process(const std::string &action, const std::vector<std::string> &args) const {
        if (action == "help" || action == "--help" || action == "-h") {
            return PrintModuleHelp("eap", {
                                           {"create-application", "Define a new application from an artifact in an ESM bucket"},
                                           {"update-application", "Change an existing application's definition"},
                                           {"list-applications", "List the defined applications and how many instances are running"},
                                           {"get-application", "Show one application's definition"},
                                           {"delete-application", "Delete an application definition"},
                                           {"start-application", "Ask the manager to start an application"},
                                           {"stop-application", "Ask the manager to stop an application"},
                                   });
        }

        // Every eap action decides which code euclid executes and under whose identity, so all of
        // them are administrator-only. Checked here, once, ahead of dispatch - a client-side
        // fail-fast for a better error message only, not the security boundary: the server
        // re-checks every request regardless of what an older CLI binary sends. Help is exempt,
        // since usage text gives nothing away.
        if (!IsHelpRequest(args)) {
            if (_authentication.token.empty()) {
                std::cerr << "error: " << action << " failed: not authenticated; run 'euclid-cli eam login' again\n";
                return 1;
            }
            if (!_authentication.isAdmin) {
                std::cerr << "error: " << action << " failed: administrator privileges required\n";
                return 1;
            }
        }

        if (action == "create-application") return createApplication(args);
        if (action == "update-application") return updateApplication(args);
        if (action == "list-applications") return listApplications(args);
        if (action == "get-application") return getApplication(args);
        if (action == "delete-application") return deleteApplication(args);
        if (action == "start-application") return setState(args, true);
        if (action == "stop-application") return setState(args, false);

        std::cerr << "error: unknown eap action '" << action << "'\n";
        return 1;
    }

    int EapCli::createApplication(const std::vector<std::string> &args) const {
        po::options_description desc("create a new application");
        desc.add_options()
                ("application-id,n", po::value<std::string>()->required(), "name identifying the application, unique across the installation")
                ("runtime,r", po::value<std::string>()->required(), "runtime the artifact is started with: JAVA, PYTHON, NODEJS or BINARY")
                ("bucket,b", po::value<std::string>()->required(), "name of the ESM bucket holding the artifact")
                ("artifact,a", po::value<std::string>()->required(), "object key of the artifact within that bucket")
                ("user,u", po::value<std::string>()->required(), "EAM user the application runs as; its access key is what the application signs its own calls with")
                ("command,c", po::value<std::string>(), "command to run instead of the runtime's default")
                ("arguments", po::value<std::string>(), "comma-separated arguments passed after the artifact")
                ("environment,e", po::value<std::string>(), "comma-separated KEY=value environment variables")
                ("min-instances", po::value<long>(), "smallest number of instances the autoscaler keeps running (default 1)")
                ("max-instances", po::value<long>(), "largest number of instances the autoscaler may scale out to (default 1)")
                ("ready-timeout", po::value<long>(), "how long an instance may take to create its socket, in milliseconds (default 30000)");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "create-application",
                                   "--application-id <name> --runtime <runtime> --bucket <bucket> --artifact <key> --user <user> "
                                   "[--command <cmd>] [--arguments <list>] [--environment <list>] [--min-instances <n>] "
                                   "[--max-instances <n>] [--ready-timeout <ms>]",
                                   "Defines a new application from an artifact already stored in an ESM bucket - upload it first with "
                                   "\"esm upload-file\", or through a transfer server. The manager copies the artifact to the host, "
                                   "starts it with the runtime's interpreter (java -jar, python3, node) or directly for BINARY, and "
                                   "scales it between --min-instances and --max-instances like any other module. The process is handed "
                                   "its socket path in EUCLID_SOCKET and the access key of --user in EUCLID_ACCESS_KEY_ID/"
                                   "EUCLID_SECRET_ACCESS_KEY, which it uses to sign its own calls back into euclid (RFC 9421). "
                                   "The application is created stopped - use \"eap start-application\" to run it.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        boost::json::object request{
                {"applicationId", vm["application-id"].as<std::string>()},
                {"runtime", vm["runtime"].as<std::string>()},
                {"bucket", vm["bucket"].as<std::string>()},
                {"artifact", vm["artifact"].as<std::string>()},
                {"user", vm["user"].as<std::string>()}
        };
        if (vm.contains("command")) request["command"] = vm["command"].as<std::string>();
        if (vm.contains("arguments")) request["arguments"] = SplitList(vm["arguments"].as<std::string>());
        if (vm.contains("environment")) request["environment"] = SplitEnvironment(vm["environment"].as<std::string>());
        if (vm.contains("min-instances")) request["minInstances"] = vm["min-instances"].as<long>();
        if (vm.contains("max-instances")) request["maxInstances"] = vm["max-instances"].as<long>();
        if (vm.contains("ready-timeout")) request["readyTimeoutMs"] = vm["ready-timeout"].as<long>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eap", "create-application", request);
            if (!response.IsSuccess()) {
                std::cerr << "error: create-application failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EapCli::updateApplication(const std::vector<std::string> &args) const {
        po::options_description desc("update an existing application");
        desc.add_options()
                ("application-id,n", po::value<std::string>()->required(), "name of the application to change")
                ("runtime,r", po::value<std::string>(), "runtime the artifact is started with")
                ("artifact,a", po::value<std::string>(), "object key of the artifact within the application's bucket")
                ("command,c", po::value<std::string>(), "command to run instead of the runtime's default")
                ("arguments", po::value<std::string>(), "comma-separated arguments; replaces the current list")
                ("environment,e", po::value<std::string>(), "comma-separated KEY=value environment variables; replaces the current set")
                ("min-instances", po::value<long>(), "smallest number of instances the autoscaler keeps running")
                ("max-instances", po::value<long>(), "largest number of instances the autoscaler may scale out to")
                ("ready-timeout", po::value<long>(), "how long an instance may take to create its socket, in milliseconds");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "update-application",
                                   "--application-id <name> [--runtime <runtime>] [--artifact <key>] [--command <cmd>] "
                                   "[--arguments <list>] [--environment <list>] [--min-instances <n>] [--max-instances <n>] "
                                   "[--ready-timeout <ms>]",
                                   "Changes an existing application's definition. Only the options actually given are altered, so one "
                                   "setting can be changed without resending the whole definition; --arguments and --environment "
                                   "replace the current values rather than adding to them. A running application keeps running on its "
                                   "old definition until it is restarted - \"eap stop-application\" followed by "
                                   "\"eap start-application\" applies the change, which is also how a newly uploaded artifact is "
                                   "picked up.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        boost::json::object request{{"applicationId", vm["application-id"].as<std::string>()}};
        if (vm.contains("runtime")) request["runtime"] = vm["runtime"].as<std::string>();
        if (vm.contains("artifact")) request["artifact"] = vm["artifact"].as<std::string>();
        if (vm.contains("command")) request["command"] = vm["command"].as<std::string>();
        if (vm.contains("arguments")) request["arguments"] = SplitList(vm["arguments"].as<std::string>());
        if (vm.contains("environment")) request["environment"] = SplitEnvironment(vm["environment"].as<std::string>());
        if (vm.contains("min-instances")) request["minInstances"] = vm["min-instances"].as<long>();
        if (vm.contains("max-instances")) request["maxInstances"] = vm["max-instances"].as<long>();
        if (vm.contains("ready-timeout")) request["readyTimeoutMs"] = vm["ready-timeout"].as<long>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eap", "update-application", request);
            if (!response.IsSuccess()) {
                std::cerr << "error: update-application failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EapCli::listApplications(const std::vector<std::string> &args) const {
        po::options_description desc("list applications");
        desc.add_options()
                ("prefix,p", po::value<std::string>(), "only list applications whose ID starts with this");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "list-applications", "[--prefix <prefix>]",
                                   "Lists the defined applications: their runtime, artifact, autoscaler bounds, the state they should "
                                   "be in, and how many instances the manager currently has running.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        boost::json::object request;
        if (vm.contains("prefix")) request["prefix"] = vm["prefix"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eap", "list-applications", request);
            if (!response.IsSuccess()) {
                std::cerr << "error: list-applications failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EapCli::getApplication(const std::vector<std::string> &args) const {
        po::options_description desc("show an application");
        desc.add_options()
                ("application-id,n", po::value<std::string>()->required(), "name of the application");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "get-application", "--application-id <name>",
                                   "Shows one application's definition and how many instances of it are running.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eap", "get-application", boost::json::object{{"applicationId", vm["application-id"].as<std::string>()}});
            if (!response.IsSuccess()) {
                std::cerr << "error: get-application failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EapCli::deleteApplication(const std::vector<std::string> &args) const {
        po::options_description desc("delete an application");
        desc.add_options()
                ("application-id,n", po::value<std::string>()->required(), "name of the application to delete");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "delete-application", "--application-id <name>",
                                   "Deletes an application definition. Any instances the manager is running are stopped on its next "
                                   "reconcile - an application that no longer exists is not something it keeps running. The artifact "
                                   "in the bucket is left alone.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eap", "delete-application", boost::json::object{{"applicationId", vm["application-id"].as<std::string>()}});
            if (!response.IsSuccess()) {
                std::cerr << "error: delete-application failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EapCli::setState(const std::vector<std::string> &args, const bool start) const {
        const std::string action = start ? "start-application" : "stop-application";

        po::options_description desc(start ? "start an application" : "stop an application");
        desc.add_options()
                ("application-id,n", po::value<std::string>()->required(), "name of the application");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", action, "--application-id <name>",
                                   start ? "Records that this application should be running. The manager's reconciler picks that up "
                                           "within a few seconds, materialises the artifact from its bucket and starts it; from then "
                                           "on the autoscaler runs between the application's minimum and maximum instance counts."
                                         : "Records that this application should be stopped. The manager's reconciler stops every "
                                           "instance on its next pass. The definition and the artifact are left in place, so "
                                           "\"start-application\" brings it back as it was.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eap", action, boost::json::object{{"applicationId", vm["application-id"].as<std::string>()}});
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

}// namespace Euclid::CLI
