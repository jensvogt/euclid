// C++ includes
#include <algorithm>
#include <cctype>
#include <sstream>

// Euclid includes
#include <euclid/cli/eag/EagCli.h>

namespace Euclid::CLI {

    namespace po = boost::program_options;

    namespace {

        // Sends one route-management request and prints whatever comes back. Every action here is
        // the same shape - build an object, post it, print it - so the only thing worth writing
        // per action is the object itself.
        int Send(const std::string &endpoint, const Credentials::Entry &authentication, const std::string &caCertPath,
                 const bool pretty, const std::string &action, const boost::json::object &request) {
            try {
                const HttpClient client(endpoint, authentication, caCertPath);
                const HttpResponse response = client.Post("eag", action, request);
                if (!response.IsSuccess()) {
                    std::cerr << "error: " << action << " failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                    return 1;
                }
                Core::WriteJson(std::cout, response.body, pretty);
                return 0;
            } catch (const std::exception &ex) {
                std::cerr << "error: " << ex.what() << std::endl;
                return 1;
            }
        }

        // Caught here rather than server-side, because a path typed without its leading slash is
        // the mistake somebody actually makes, and the answer should name the argument.
        bool ValidPath(const std::string &path, const std::string &action) {
            if (path.starts_with("/")) return true;
            std::cerr << "error: " << action << " failed: --path must start with '/', e.g. /resource\n";
            return false;
        }

        // Splits a comma-separated method list into upper-cased entries, e.g. "get, post" -> two.
        // Empty stays empty, which is what the server reads as "every method".
        boost::json::array SplitMethods(const std::string &value) {
            boost::json::array methods;
            std::stringstream ss(value);
            for (std::string part; std::getline(ss, part, ',');) {
                const auto first = part.find_first_not_of(" \t");
                const auto last = part.find_last_not_of(" \t");
                if (first == std::string::npos) continue;

                auto method = part.substr(first, last - first + 1);
                std::ranges::transform(method, method.begin(), [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });
                methods.push_back(boost::json::string(method));
            }
            return methods;
        }

        bool ValidAuthentication(const std::string &authentication, const std::string &action) {
            if (authentication == "none" || authentication == "euclid" || authentication == "basic") return true;
            std::cerr << "error: " << action << " failed: --authentication must be \"none\", \"euclid\" or \"basic\"\n";
            return false;
        }

    }// namespace

    EagCli::EagCli(std::string endpoint, Credentials::Entry authentication, const bool pretty, std::string caCertPath) : _endpoint(std::move(endpoint)), _authentication(std::move(authentication)), _pretty(pretty), _caCertPath(std::move(caCertPath)) {}

    int EagCli::process(const std::string &action, const std::vector<std::string> &args) const {
        if (action == "help" || action == "--help" || action == "-h") {
            return PrintModuleHelp("eag", {
                                           {"create-route", "Publish a path through the gateway, pointing it at an application"},
                                           {"delete-route", "Remove a route, taking its path out of service"},
                                           {"get-route", "Show one route's definition"},
                                           {"list-routes", "List the configured routes"},
                                           {"update-route", "Change an existing route's path, application or authentication"},
                                   });
        }

        // Administrator-only, for the same reason ets is: a route decides what the installation
        // exposes to the outside world and whether reaching it needs a credential at all. Checked
        // here for a clearer message than the 403 the server would return; the server checks
        // regardless, so this is a fail-fast and not the security boundary.
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

        if (action == "create-route") return createRoute(args);
        if (action == "update-route") return updateRoute(args);
        if (action == "list-routes") return listRoutes(args);
        if (action == "get-route") return getRoute(args);
        if (action == "delete-route") return deleteRoute(args);

        std::cerr << "error: unknown eag action '" << action << "'\n";
        return 1;
    }

    int EagCli::createRoute(const std::vector<std::string> &args) const {
        po::options_description desc("create a route");
        desc.add_options()
                ("route-id,r", po::value<std::string>()->required(), "name for the route, unique within the installation")
                ("path,p", po::value<std::string>()->required(), "path prefix to publish, e.g. /resource")
                ("application,a", po::value<std::string>(), "application the requests are sent to")
                ("module,M", po::value<std::string>(), "euclid module the requests are sent to instead of an application, e.g. eam")
                ("action", po::value<std::string>(), "the one action the module answers for on this route, e.g. login")
                ("methods,m", po::value<std::string>(), "comma-separated HTTP methods this route answers for, e.g. GET,POST; omit for all of them")
                ("region,R", po::value<std::string>(), "region requests on this route act in; defaults to your own")
                ("namespace,N", po::value<std::string>(), "namespace requests on this route act in; defaults to the one you are working in")
                ("authentication,A", po::value<std::string>()->default_value("none"), "none (anybody may call it), euclid (a bearer token, RFC 9421 signature or SigV4) or basic (HTTP Basic against a euclid user password)")
                ("inactive,i", po::bool_switch(), "create the route but leave it out of service until update-route activates it");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eag", "create-route",
                                   "--route-id <id> --path <prefix> (--application <name> | --module <name> --action <name>) "
                                   "[--methods <list>] [--region <name>] [--namespace <name>] "
                                   "[--authentication none|euclid|basic] [--inactive]",
                                   "Publishes a path prefix on the gateway's port and sends everything beneath it to "
                                   "an application, one instance at a time in turn. The path is forwarded unchanged, "
                                   "so a route on /resource reaches the application as "
                                   "/resource/searchById?id=123 - the application sees the URL its callers "
                                   "typed, and nothing about the route appears in it. "
                                   "The instances are found from the ports the manager handed them, so an application "
                                   "that scales from one instance to eight is spread across eight backends within a "
                                   "few seconds without the route being touched. "
                                   "Longest prefix wins: a route on /resource carries everything below it, and "
                                   "a later, more specific /resource/export takes precedence for its own "
                                   "subtree. A prefix only matches whole segments, so /resource never claims "
                                   "/resource-intern. "
                                   "--module publishes a euclid module's own action instead of an application: "
                                   "\"--module eam --action login\" makes the named path a login endpoint, which is what "
                                   "lets something outside euclid get a token without also having to reach euclid's "
                                   "own gateway on another port. One action per route, deliberately - a route that "
                                   "took its action from the path would publish every action the module has, "
                                   "including the ones that delete things. The request goes to euclid's own gateway, "
                                   "since a module listens on a socket nothing outside the host can reach. "
                                   "The route carries the region and namespace its requests act in, and the gateway "
                                   "puts them on every request it forwards - so a browser or a curl script needs to "
                                   "know nothing about euclid's own headers. They default to yours, and are worth "
                                   "naming when they differ: a route published for one namespace should not act in "
                                   "another just because an administrator of the first configured it. A caller that "
                                   "does send them keeps what it sent, and is refused if it is not permitted, rather "
                                   "than being quietly moved. "
                                   "Without --methods the route answers for every method. Naming some restricts it to "
                                   "those, and anything else on the path is answered with 405 and an Allow header "
                                   "rather than 404 - the caller is told the resource is there and they asked the "
                                   "wrong way round. Two routes may share a path as long as their methods do not "
                                   "overlap, which is how reads and writes of one resource can go to different "
                                   "applications without the caller seeing a seam. "
                                   "With --authentication euclid the gateway requires a euclid credential - a bearer "
                                   "token from \"eam login\", an RFC 9421 signature or SigV4 - and refuses the request "
                                   "before any application sees it. With basic it requires HTTP Basic instead, checked "
                                   "against the same euclid user passwords, and answers an unauthenticated caller with "
                                   "WWW-Authenticate so a browser prompts for a username and password; that is the one "
                                   "to reach for when the caller is a person at a browser or a script with nothing but "
                                   "curl. With none it forwards everything, which is what a route behind an external "
                                   "gateway or an application doing its own authentication wants.",
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

        const auto path = vm["path"].as<std::string>();
        const auto authentication = vm["authentication"].as<std::string>();
        if (!ValidPath(path, "create-route")) return 1;
        if (!ValidAuthentication(authentication, "create-route")) return 1;

        const auto hasApplication = vm.contains("application");
        const auto hasModule = vm.contains("module");
        if (hasApplication == hasModule) {
            std::cerr << "error: create-route failed: name either --application or --module, not both and not neither\n";
            return 1;
        }
        if (hasModule && !vm.contains("action")) {
            std::cerr << "error: create-route failed: --module also needs --action\n";
            return 1;
        }

        const boost::json::object request{
                {"routeId", vm["route-id"].as<std::string>()},
                {"path", path},
                {"applicationId", hasApplication ? vm["application"].as<std::string>() : std::string()},
                {"moduleTarget", hasModule ? vm["module"].as<std::string>() : std::string()},
                {"moduleAction", vm.contains("action") ? vm["action"].as<std::string>() : std::string()},
                {"region", vm.contains("region") ? vm["region"].as<std::string>() : std::string()},
                {"namespace", vm.contains("namespace") ? vm["namespace"].as<std::string>() : std::string()},
                {"methods", vm.contains("methods") ? SplitMethods(vm["methods"].as<std::string>()) : boost::json::array{}},
                {"authentication", authentication},
                {"active", !vm["inactive"].as<bool>()}
        };

        return Send(_endpoint, _authentication, _caCertPath, _pretty, "create-route", request);
    }

    int EagCli::updateRoute(const std::vector<std::string> &args) const {
        po::options_description desc("update a route");
        desc.add_options()
                ("route-id,r", po::value<std::string>()->required(), "route to change")
                ("path,p", po::value<std::string>(), "new path prefix")
                ("application,a", po::value<std::string>(), "application the requests are sent to")
                ("module,M", po::value<std::string>(), "euclid module the requests are sent to instead of an application")
                ("action", po::value<std::string>(), "the action the module answers for on this route")
                ("methods,m", po::value<std::string>(), "comma-separated HTTP methods, or an empty string for all of them")
                ("region,R", po::value<std::string>(), "region requests on this route act in")
                ("namespace,N", po::value<std::string>(), "namespace requests on this route act in")
                ("authentication,A", po::value<std::string>(), "none, euclid or basic")
                ("active", po::value<bool>(), "true to put the route into service, false to take it out");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eag", "update-route",
                                   "--route-id <id> [--path <prefix>] [--application <name>] [--methods <list>] "
                                   "[--authentication none|euclid|basic] [--active true|false]",
                                   "Changes an existing route. Anything not named is left as it is, so one setting can "
                                   "be changed without restating the rest. "
                                   "The gateway re-reads its routes every few seconds, so a change takes effect "
                                   "shortly after this returns rather than at the next restart. "
                                   "--methods replaces the whole list rather than adding to it, and \"--methods ''\" "
                                   "puts the route back to answering for every method. "
                                   "--active false is the way to take a path out of service without losing its "
                                   "definition: the gateway stops carrying it and answers 404, and --active true puts "
                                   "it back exactly as it was.",
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

        boost::json::object request{{"routeId", vm["route-id"].as<std::string>()}};

        if (vm.contains("path")) {
            const auto path = vm["path"].as<std::string>();
            if (!ValidPath(path, "update-route")) return 1;
            request["path"] = path;
        }
        if (vm.contains("application")) request["applicationId"] = vm["application"].as<std::string>();
        if (vm.contains("module")) request["moduleTarget"] = vm["module"].as<std::string>();
        if (vm.contains("action")) request["moduleAction"] = vm["action"].as<std::string>();
        if (vm.contains("region")) request["region"] = vm["region"].as<std::string>();
        if (vm.contains("namespace")) request["namespace"] = vm["namespace"].as<std::string>();
        if (vm.contains("methods")) request["methods"] = SplitMethods(vm["methods"].as<std::string>());
        if (vm.contains("authentication")) {
            const auto authentication = vm["authentication"].as<std::string>();
            if (!ValidAuthentication(authentication, "update-route")) return 1;
            request["authentication"] = authentication;
        }
        if (vm.contains("active")) request["active"] = vm["active"].as<bool>();

        return Send(_endpoint, _authentication, _caCertPath, _pretty, "update-route", request);
    }

    int EagCli::listRoutes(const std::vector<std::string> &args) const {
        po::options_description desc("list routes");
        desc.add_options()
                ("prefix,p", po::value<std::string>(), "only routes whose path starts with this");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eag", "list-routes", "[--prefix <path>]",
                                   "Lists the configured routes with their paths, applications, authentication and "
                                   "whether they are in service. "
                                   "Inactive routes are listed too - they are configuration that exists but is not "
                                   "being served, and leaving them out would make a route somebody deactivated look "
                                   "deleted.",
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

        return Send(_endpoint, _authentication, _caCertPath, _pretty, "list-routes", request);
    }

    int EagCli::getRoute(const std::vector<std::string> &args) const {
        po::options_description desc("get a route");
        desc.add_options()
                ("route-id,r", po::value<std::string>()->required(), "route to show");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eag", "get-route", "--route-id <id>",
                                   "Shows one route's definition: the path it publishes, the application behind it, "
                                   "what it requires of a caller, and whether it is currently in service.",
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

        return Send(_endpoint, _authentication, _caCertPath, _pretty, "get-route",
                    boost::json::object{{"routeId", vm["route-id"].as<std::string>()}});
    }

    int EagCli::deleteRoute(const std::vector<std::string> &args) const {
        po::options_description desc("delete a route");
        desc.add_options()
                ("route-id,r", po::value<std::string>()->required(), "route to delete");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eag", "delete-route", "--route-id <id>",
                                   "Deletes a route. Its path stops being served within a few seconds and callers get "
                                   "404 from then on. Nothing else is touched - the application behind it keeps "
                                   "running, and any other route pointing at it keeps working. "
                                   "To take a path out of service without losing how it was configured, use "
                                   "\"update-route --active false\" instead.",
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

        Send(_endpoint, _authentication, _caCertPath, _pretty, "delete-route",
             boost::json::object{{"routeId", vm["route-id"].as<std::string>()}});
        return 0;
    }

}// namespace Euclid::CLI