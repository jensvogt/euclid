#include <euclid/cli/eam/EamCli.h>

namespace Euclid::CLI {

    namespace po = boost::program_options;

    namespace {

        // Reports a failed response. A 401 means the stored bearer token was missing, invalid or
        // expired - the server doesn't say which, so the message stays generic, but the stale
        // token is cleared so the *next* command fails the same obvious way instead of silently
        // reusing a dead token.
        void reportFailure(const std::string_view action, const HttpResponse &response) {
            if (response.statusCode == 401) {
                Credentials::ClearToken();
                std::cerr << "error: " << action << " failed: not authenticated - your session is missing, invalid or expired; run 'euclid-cli eam login' again\n";
                return;
            }
            std::cerr << "error: " << action << " failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
        }

    }// namespace

    EamCli::EamCli(std::string endpoint, Credentials::Entry authentication, const bool pretty, std::string caCertPath) : _endpoint(std::move(endpoint)), _authentication(std::move(authentication)), _pretty(pretty), _caCertPath(std::move(caCertPath)) {}

    int EamCli::process(const std::string &action, const std::vector<std::string> &args) const {
        if (action == "help" || action == "--help" || action == "-h") {
            return PrintModuleHelp("eam", {
                                           {"login", "Authenticate and store a bearer token and SigV4 access key"},
                                           {"register", "Register a new user account"},
                                           {"list-users", "List user accounts"},
                                           {"delete-user", "Delete a user account"},
                                           {"create-access-key", "Create a SigV4 access key and store it locally"},
                                           {"list-access-keys", "List your access keys"},
                                           {"delete-access-key", "Delete one of your access keys"},
                                   });
        }
        if (action == "login") {
            return login(args);
        }
        if (action == "register") {
            return registerUser(args);
        }
        if (action == "list-users") {
            return listUsers(args);
        }
        if (action == "delete-user") {
            return deleteUser(args);
        }
        if (action == "create-access-key") {
            return createAccessKey(args);
        }
        if (action == "list-access-keys") {
            return listAccessKeys(args);
        }
        if (action == "delete-access-key") {
            return deleteAccessKey(args);
        }
        std::cerr << "error: unknown eam action '" << action << "'\n";
        return 1;
    }

    int EamCli::login(const std::vector<std::string> &args) const {
        po::options_description desc("eam login options");
        desc.add_options()
                ("user,u", po::value<std::string>()->required(), "username")
                ("password,p", po::value<std::string>()->required(), "password");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eam", "login", "--user <username> --password <password>",
                                   "Authenticates against the Euclid access module and, on success, stores the "
                                   "returned bearer token locally so it is used automatically to authenticate "
                                   "subsequent commands. Also provisions a SigV4 access key on first login (or "
                                   "reuses the existing one on later logins) and stores it alongside the token, "
                                   "so a separate 'create-access-key' call is not needed for AWS-service commands.",
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

        Dto::EAM::LoginRequest request;
        request.userId = vm["user"].as<std::string>();
        request.password = vm["password"].as<std::string>();

        try {
            const HttpClient client(_endpoint, {}, _caCertPath);
            const HttpResponse response = client.Post("eam", "login", boost::json::value_from(request));

            if (!response.IsSuccess()) {
                std::cerr << "error: login failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }

            const auto loginResponse = boost::json::value_to<Dto::EAM::LoginResponse>(response.body);
            if (loginResponse.token.empty()) {
                std::cerr << "error: login response did not contain a token\n";
                return 1;
            }
            Credentials::Save({
                    .token = loginResponse.token,
                    .userId = loginResponse.user,
                    .accountId = loginResponse.accountId,
                    .region = loginResponse.region,
                    .accessKeyId = loginResponse.accessKeyId,
                    .secretAccessKey = loginResponse.secretAccessKey,
                    .isAdmin = loginResponse.isAdmin,
            });

            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EamCli::registerUser(const std::vector<std::string> &args) const {
        po::options_description desc("eam register options");
        desc.add_options()
                ("region,r", po::value<std::string>()->required(), "region name")
                ("account-id,a", po::value<std::string>()->required(), "account ID")
                ("user,u", po::value<std::string>()->required(), "username")
                ("email,e", po::value<std::string>()->required(), "email")
                ("password,p", po::value<std::string>()->required(), "password");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eam", "register", "--user <username> --email <email> --password <password> --region <region> --account-id <account-id>",
                                   "Registers a new user account with the Euclid access module.",
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

        Dto::EAM::RegisterRequest request;
        request.region = vm["region"].as<std::string>();
        request.accountId = vm["account-id"].as<std::string>();
        request.userId = vm["user"].as<std::string>();
        request.password = vm["password"].as<std::string>();
        request.email = vm["email"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eam", "register", boost::json::value_from(request));

            if (!response.IsSuccess()) {
                reportFailure("register", response);
                return 1;
            }

            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EamCli::listUsers(const std::vector<std::string> &args) const {
        po::options_description desc("eam list users");
        desc.add_options()
                ("prefix,p", po::value<std::string>(), "user name prefix")
                ("pageSize,s", po::value<long>()->default_value(-1), "page size")
                ("pageIndex,i", po::value<long>()->default_value(-1), "page index")
                ("sortColumn,c", po::value<std::string>()->default_value("userId"), "sort column");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eam", "list-users", "[--prefix <prefix>] [--pageSize <n>] [--pageIndex <n>] [--sortColumn <column>]",
                                   "Lists user accounts, optionally filtered by name prefix and paginated.",
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

        Dto::EAM::ListUserRequest request;
        if (vm.contains("prefix")) {
            request.prefix = vm["prefix"].as<std::string>();
        }
        request.pageSize = vm["pageSize"].as<long>();
        request.pageIndex = vm["pageIndex"].as<long>();
        request.sortColumn = vm["sortColumn"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eam", "list-users", boost::json::value_from(request));

            if (!response.IsSuccess()) {
                reportFailure("list-users", response);
                return 1;
            }

            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EamCli::deleteUser(const std::vector<std::string> &args) const {
        po::options_description desc("eam delete user");
        desc.add_options()
                ("userId,u", po::value<std::string>()->required(), "user ID");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eam", "delete-user", "--userId <userId>",
                                   "Deletes an existing user account.",
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

        Dto::EAM::DeleteUserRequest request;
        request.userId = vm["userId"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);

            if (const HttpResponse response = client.Post("eam", "delete-user", boost::json::value_from(request)); !response.IsSuccess()) {
                reportFailure("delete-user", response);
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EamCli::createAccessKey(const std::vector<std::string> &args) const {
        if (IsHelpRequest(args)) {
            return PrintActionHelp("eam", "create-access-key", "",
                                   "Creates a new SigV4 access key for the logged-in user and stores it locally "
                                   "so subsequent AWS-service commands sign requests with it instead of the "
                                   "bearer token. The secret is only ever shown once, right here.",
                                   {});
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eam", "create-access-key", boost::json::value());

            if (!response.IsSuccess()) {
                reportFailure("create-access-key", response);
                return 1;
            }

            const auto created = boost::json::value_to<Dto::EAM::CreateAccessKeyResponse>(response.body);

            auto entry = _authentication;
            entry.accessKeyId = created.accessKeyId;
            entry.secretAccessKey = created.secretAccessKey;
            Credentials::Save(entry);

            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EamCli::listAccessKeys(const std::vector<std::string> &args) const {
        if (IsHelpRequest(args)) {
            return PrintActionHelp("eam", "list-access-keys", "",
                                   "Lists the logged-in user's own access keys. Secrets are never returned "
                                   "after creation.",
                                   {});
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eam", "list-access-keys", boost::json::value());

            if (!response.IsSuccess()) {
                reportFailure("list-access-keys", response);
                return 1;
            }

            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EamCli::deleteAccessKey(const std::vector<std::string> &args) const {
        po::options_description desc("eam delete access key");
        desc.add_options()
                ("accessKeyId,k", po::value<std::string>()->required(), "access key ID");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eam", "delete-access-key", "--accessKeyId <accessKeyId>",
                                   "Deletes one of the logged-in user's own access keys.",
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

        Dto::EAM::DeleteAccessKeyRequest request;
        request.accessKeyId = vm["accessKeyId"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);

            if (const HttpResponse response = client.Post("eam", "delete-access-key", boost::json::value_from(request)); !response.IsSuccess()) {
                reportFailure("delete-access-key", response);
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }
}