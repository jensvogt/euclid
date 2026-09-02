// C++ includes
#include <sstream>

// Euclid includes
#include <euclid/cli/ets/EtsCli.h>

namespace Euclid::CLI {

    namespace po = boost::program_options;

    namespace {

        // Splits a comma-separated option value into trimmed, non-empty entries, e.g.
        // "alice, bob ,carol" -> ["alice", "bob", "carol"]. Used for --users and --groups, which
        // are lists on the wire but a single comma-separated argument on the command line.
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

    EtsCli::EtsCli(std::string endpoint, Credentials::Entry authentication, const bool pretty, std::string caCertPath) : _endpoint(std::move(endpoint)), _authentication(std::move(authentication)), _pretty(pretty), _caCertPath(std::move(caCertPath)) {}

    int EtsCli::process(const std::string &action, const std::vector<std::string> &args) const {
        if (action == "help" || action == "--help" || action == "-h") {
            return PrintModuleHelp("ets", {
                                           {"create-server", "Define a new FTP or SFTP transfer server for an ESM bucket"},
                                           {"update-server", "Change an existing transfer server's settings"},
                                           {"list-servers", "List the defined transfer servers and their state"},
                                           {"get-server", "Show one transfer server's definition"},
                                           {"delete-server", "Delete a transfer server definition"},
                                           {"start-server", "Ask the manager to start a transfer server"},
                                           {"stop-server", "Ask the manager to stop a transfer server"},
                                   });
        }

        // Every ets action is administrator-only, since each one changes (or reveals) which
        // network listeners exist and who can reach which bucket through them. Checked here, once,
        // ahead of dispatch, so a new action added later does not have to remember it. This is a
        // client-side fail-fast for a better error message only, not the security boundary - the
        // server re-checks on every request and would reject it regardless of what an older or
        // modified CLI binary sends.
        //
        // Help is deliberately exempt: an action's usage text gives nothing away, and someone who
        // is not logged in yet is exactly the person who needs to be able to read it.
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

        if (action == "create-server") return createServer(args);
        if (action == "update-server") return updateServer(args);
        if (action == "list-servers") return listServers(args);
        if (action == "get-server") return getServer(args);
        if (action == "delete-server") return deleteServer(args);
        if (action == "start-server" || action == "stop-server") return setServerState(action, args);

        std::cerr << "error: unknown ets action '" << action << "'\n";
        return 1;
    }

    int EtsCli::createServer(const std::vector<std::string> &args) const {
        po::options_description desc("create a new transfer server");
        desc.add_options()
                ("server-id,n", po::value<std::string>()->required(), "name identifying the server, unique across the installation")
                ("protocol,P", po::value<std::string>()->default_value("SFTP"), "transfer protocol, FTP or SFTP")
                ("port,p", po::value<long>()->required(), "TCP port to listen on; must not be used by another transfer server")
                ("bucket,b", po::value<std::string>()->required(), "name of the ESM bucket this server's clients read and write")
                ("address,a", po::value<std::string>()->default_value("0.0.0.0"), "address to bind to")
                ("home-dir,H", po::value<std::string>(), "key prefix each client is rooted at, with {user} standing for the login name, e.g. \"{user}\"; default is the bucket root, shared by every client")
                ("users,u", po::value<std::string>(), "comma-separated EAM user IDs allowed to log in")
                ("groups,g", po::value<std::string>(), "comma-separated EAM user groups whose members may log in")
                ("host-key,k", po::value<std::string>(), "SFTP only: private SSH host key file; generated on first start if absent")
                ("pasv-min", po::value<long>(), "FTP only: lowest passive data port (default 6000)")
                ("pasv-max", po::value<long>(), "FTP only: highest passive data port (default 6100)");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ets", "create-server",
                                   "--server-id <name> --port <port> --bucket <bucket> [--protocol FTP|SFTP] "
                                   "[--address <addr>] [--home-dir <prefix>] [--users <list>] [--groups <list>] "
                                   "[--host-key <path>] [--pasv-min <port>] [--pasv-max <port>]",
                                   "Defines a new FTP or SFTP transfer server fronting an ESM bucket. Clients "
                                   "authenticate with their EAM credentials and are admitted if they are named by "
                                   "--users or belong to one of --groups; whatever they upload is stored as an object "
                                   "in the bucket, which is what every listing and download then reads back. "
                                   "Without --home-dir every client shares one flat key space at the bucket root, so "
                                   "the key of an upload is exactly the path the client typed; --home-dir \"{user}\" "
                                   "roots each session at a prefix named after the user that logged in, which is what "
                                   "keeps one client's files out of another's and puts the delivering user into the "
                                   "object key. "
                                   "The server is created stopped - use \"ets start-server\" to run it.",
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

        // Nobody could log in to a server that names neither users nor groups, so this is almost
        // certainly a mistake rather than an intentionally unreachable server.
        if (!vm.contains("users") && !vm.contains("groups")) {
            std::cerr << "error: create-server failed: at least one of --users or --groups is required, "
                         "otherwise nobody can log in\n";
            return 1;
        }

        boost::json::object request{
                {"serverId", vm["server-id"].as<std::string>()},
                {"protocol", vm["protocol"].as<std::string>()},
                {"port", vm["port"].as<long>()},
                {"bucket", vm["bucket"].as<std::string>()},
                {"address", vm["address"].as<std::string>()}
        };
        if (vm.contains("home-dir")) request["homeDirectory"] = vm["home-dir"].as<std::string>();
        if (vm.contains("users")) request["userIds"] = SplitList(vm["users"].as<std::string>());
        if (vm.contains("groups")) request["userGroups"] = SplitList(vm["groups"].as<std::string>());
        if (vm.contains("host-key")) request["hostKey"] = vm["host-key"].as<std::string>();
        if (vm.contains("pasv-min")) request["pasvMin"] = vm["pasv-min"].as<long>();
        if (vm.contains("pasv-max")) request["pasvMax"] = vm["pasv-max"].as<long>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ets", "create-server", request);
            if (!response.IsSuccess()) {
                std::cerr << "error: create-server failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EtsCli::updateServer(const std::vector<std::string> &args) const {
        po::options_description desc("update an existing transfer server");
        desc.add_options()
                ("server-id,n", po::value<std::string>()->required(), "name of the server to change")
                ("port,p", po::value<long>(), "TCP port to listen on")
                ("bucket,b", po::value<std::string>(), "name of the ESM bucket this server's clients read and write")
                ("address,a", po::value<std::string>(), "address to bind to")
                ("home-dir,H", po::value<std::string>(), "key prefix each client is rooted at, with {user} standing for the login name; pass an empty string to move back to the bucket root")
                ("users,u", po::value<std::string>(), "comma-separated EAM user IDs allowed to log in; replaces the current list")
                ("groups,g", po::value<std::string>(), "comma-separated EAM user groups whose members may log in; replaces the current list")
                ("host-key,k", po::value<std::string>(), "SFTP only: private SSH host key file")
                ("pasv-min", po::value<long>(), "FTP only: lowest passive data port")
                ("pasv-max", po::value<long>(), "FTP only: highest passive data port");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ets", "update-server",
                                   "--server-id <name> [--port <port>] [--bucket <bucket>] [--address <addr>] "
                                   "[--home-dir <prefix>] [--users <list>] [--groups <list>] [--host-key <path>] "
                                   "[--pasv-min <port>] [--pasv-max <port>]",
                                   "Changes an existing transfer server's settings. Only the options actually given "
                                   "are altered, so one setting can be changed without resending the whole definition; "
                                   "--users and --groups replace the current list rather than adding to it. Changing "
                                   "--home-dir changes where new uploads land and what a client sees, but moves no "
                                   "object that is already in the bucket. A server "
                                   "that is running keeps running on its old settings until it is restarted - "
                                   "\"ets stop-server\" followed by \"ets start-server\" applies them. "
                                   "The protocol cannot be changed; delete the server and create it again instead.",
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

        boost::json::object request{{"serverId", vm["server-id"].as<std::string>()}};
        if (vm.contains("port")) request["port"] = vm["port"].as<long>();
        if (vm.contains("bucket")) request["bucket"] = vm["bucket"].as<std::string>();
        if (vm.contains("address")) request["address"] = vm["address"].as<std::string>();
        if (vm.contains("home-dir")) request["homeDirectory"] = vm["home-dir"].as<std::string>();
        if (vm.contains("users")) request["userIds"] = SplitList(vm["users"].as<std::string>());
        if (vm.contains("groups")) request["userGroups"] = SplitList(vm["groups"].as<std::string>());
        if (vm.contains("host-key")) request["hostKey"] = vm["host-key"].as<std::string>();
        if (vm.contains("pasv-min")) request["pasvMin"] = vm["pasv-min"].as<long>();
        if (vm.contains("pasv-max")) request["pasvMax"] = vm["pasv-max"].as<long>();

        if (request.size() == 1) {
            std::cerr << "error: update-server failed: nothing to change; give at least one option besides --server-id\n";
            return 1;
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("ets", "update-server", request);
            if (!response.IsSuccess()) {
                std::cerr << "error: update-server failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EtsCli::listServers(const std::vector<std::string> &args) const {
        po::options_description desc("list transfer servers");
        desc.add_options()
                ("prefix,x", po::value<std::string>()->default_value(""), "only list servers whose ID starts with this prefix");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ets", "list-servers", "[--prefix <prefix>]",
                                   "Lists the defined transfer servers. Each entry carries both a desiredState - "
                                   "what \"start-server\"/\"stop-server\" last asked for - and a state, which is what "
                                   "the manager currently has running. The two differ while a server is still coming "
                                   "up or going down, and stay different if it cannot start at all.",
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
            const HttpResponse response = client.Post("ets", "list-servers", boost::json::object{{"prefix", vm["prefix"].as<std::string>()}});
            if (!response.IsSuccess()) {
                std::cerr << "error: list-servers failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EtsCli::getServer(const std::vector<std::string> &args) const {
        po::options_description desc("show a transfer server");
        desc.add_options()
                ("server-id,n", po::value<std::string>()->required(), "name of the server to show");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ets", "get-server", "--server-id <name>",
                                   "Shows one transfer server's full definition: protocol, address and port, the "
                                   "bucket it fronts, the EAM users and groups allowed to log in, and both its "
                                   "desired and observed state.",
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
            const HttpResponse response = client.Post("ets", "get-server", boost::json::object{{"serverId", vm["server-id"].as<std::string>()}});
            if (!response.IsSuccess()) {
                std::cerr << "error: get-server failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EtsCli::deleteServer(const std::vector<std::string> &args) const {
        po::options_description desc("delete a transfer server");
        desc.add_options()
                ("server-id,n", po::value<std::string>()->required(), "name of the server to delete");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ets", "delete-server", "--server-id <name>",
                                   "Deletes a transfer server definition. A server that is currently running is "
                                   "stopped, since the manager runs exactly the servers that are defined and marked "
                                   "running. Nothing in the bucket is touched - the objects clients uploaded through "
                                   "this server remain, and are still reachable through ESM or any other server "
                                   "pointed at the same bucket.",
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
            const HttpResponse response = client.Post("ets", "delete-server", boost::json::object{{"serverId", vm["server-id"].as<std::string>()}});
            if (!response.IsSuccess()) {
                std::cerr << "error: delete-server failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EtsCli::setServerState(const std::string &action, const std::vector<std::string> &args) const {
        const bool starting = action == "start-server";

        po::options_description desc(starting ? "start a transfer server" : "stop a transfer server");
        desc.add_options()
                ("server-id,n", po::value<std::string>()->required(), starting ? "name of the server to start" : "name of the server to stop");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("ets", action, "--server-id <name>",
                                   starting
                                       ? "Marks a transfer server as one that should be running. The command returns "
                                         "as soon as that intent is recorded, not once the server is up: the manager "
                                         "notices within a few seconds and spawns the process. Use \"ets list-servers\" "
                                         "to see when its observed state catches up with the desired one."
                                       : "Marks a transfer server as one that should not be running. As with "
                                         "start-server the command records the intent and returns; the manager stops "
                                         "the process shortly afterwards. The definition is kept, so the server can be "
                                         "started again later with the same settings.",
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
            const HttpResponse response = client.Post("ets", action, boost::json::object{{"serverId", vm["server-id"].as<std::string>()}});
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
