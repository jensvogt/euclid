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
                                           {"create-user-group", "Create a new user group"},
                                           {"list-user-groups", "List user groups"},
                                           {"user-group-add-user", "Add an user to an user group"},
                                           {"user-group-remove-user", "Removes an user to an user group"},
                                           {"delete-user-group", "Delete an existing user group"},
                                           {"create-account", "Create a new account"},
                                           {"list-accounts", "List accounts"},
                                           {"delete-account", "Delete an existing account"},
                                           {"create-namespace", "Create a new namespace under an account"},
                                           {"list-namespaces", "List namespaces under an account"},
                                           {"delete-namespace", "Delete an existing namespace"},
                                           {"grant-namespace-access", "Grant a user access to a namespace within an account"},
                                           {"revoke-namespace-access", "Revoke a user's access to a namespace within an account"},
                                           {"change-namespace", "Switch the active namespace for this session"},
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
        if (action == "create-user-group") {
            return createUserGroup(args);
        }
        if (action == "list-user-groups") {
            return listUserGroups(args);
        }
        if (action == "user-group-add-user") {
            return addUserToUserGroup(args);
        }
        if (action == "user-group-remove-user") {
            return removeUserFromUserGroup(args);
        }
        if (action == "delete-user-group") {
            return deleteUserGroup(args);
        }
        if (action == "create-account") {
            return createAccount(args);
        }
        if (action == "list-accounts") {
            return listAccounts(args);
        }
        if (action == "delete-account") {
            return deleteAccount(args);
        }
        if (action == "create-namespace") {
            return createNamespace(args);
        }
        if (action == "list-namespaces") {
            return listNamespaces(args);
        }
        if (action == "delete-namespace") {
            return deleteNamespace(args);
        }
        if (action == "grant-namespace-access") {
            return grantNamespaceAccess(args);
        }
        if (action == "revoke-namespace-access") {
            return revokeNamespaceAccess(args);
        }
        if (action == "change-namespace") {
            return changeNamespace(args);
        }
        std::cerr << "error: unknown eam action '" << action << "'\n";
        return 1;
    }

    int EamCli::login(const std::vector<std::string> &args) const {
        po::options_description desc("eam login options");
        desc.add_options()
                ("user,u", po::value<std::string>()->required(), "username")
                ("password,p", po::value<std::string>()->required(), "password")
                ("namespace,n", po::value<std::string>(), "namespace to make active for this session");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eam", "login", "--user <username> --password <password> [--namespace <name>]",
                                   "Authenticates against the Euclid access module and, on success, stores the "
                                   "returned bearer token locally so it is used automatically to authenticate "
                                   "subsequent commands. Also provisions a SigV4 access key on first login (or "
                                   "reuses the existing one on later logins) and stores it alongside the token, "
                                   "so a separate 'create-access-key' call is not needed for Euclid-service commands. "
                                   "If --namespace is given, it is validated and set as the session's active namespace "
                                   "(see euclid-cli-eam-change-namespace(1)) - every namespace-scoped command run "
                                   "afterward is automatically restricted to it, until changed again.",
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
            Credentials::Entry entry{
                    .token = loginResponse.token,
                    .userId = loginResponse.user,
                    .accountId = loginResponse.accountId,
                    .region = loginResponse.region,
                    .accessKeyId = loginResponse.accessKeyId,
                    .secretAccessKey = loginResponse.secretAccessKey,
                    .isAdmin = loginResponse.isAdmin,
            };

            if (vm.contains("namespace")) {
                const auto ns = vm["namespace"].as<std::string>();
                Dto::EAM::ChangeNamespaceRequest nsRequest;
                nsRequest.ns = ns;

                const HttpClient nsClient(_endpoint, entry, _caCertPath);
                if (const HttpResponse nsResponse = nsClient.Post("eam", "change-namespace", boost::json::value_from(nsRequest)); !nsResponse.IsSuccess()) {
                    Credentials::Save(entry);
                    std::cerr << "warning: logged in, but setting namespace failed (HTTP " << nsResponse.statusCode << "): " << boost::json::serialize(nsResponse.body) << std::endl;
                    return 1;
                }
                entry.nameSpace = ns;
            }

            Credentials::Save(entry);

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
                ("page-size,s", po::value<long>()->default_value(-1), "page size")
                ("page-index,i", po::value<long>()->default_value(-1), "page index")
                ("sort-column,c", po::value<std::string>()->default_value("userId"), "sort column")
                ("sort-direction,d", po::value<std::string>()->default_value("asc"), "sort direction");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eam", "list-users", "[--prefix <prefix>] [--page-size <n>] [--page-index <n>] [--sort-column <column>] [--sort-direction <direction>]",
                                   "Lists user accounts, optionally filtered by name prefix and paginated. Sort-column can be 'name', 'userId', 'email'. Sort-direction"
                                   "can be one of 'asc', 'desc'",
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
        request.pageSize = vm["page-size"].as<long>();
        request.pageIndex = vm["page-index"].as<long>();
        request.sortColumn = vm["sort-column"].as<std::string>();
        request.sortDirection = vm["sort-direction"].as<std::string>();

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
                                   "so subsequent Euclid-service commands sign requests with it instead of the "
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

    int EamCli::createUserGroup(const std::vector<std::string> &args) const {
        po::options_description desc("eam create user group options");
        desc.add_options()
                ("name,n", po::value<std::string>()->required(), "group name")
                ("description,d", po::value<std::string>()->default_value(""), "group description");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eam", "create-user-group", "--name <name> [--description <description>]",
                                   "Creates a new, empty user group. Requires administrator privileges.",
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

        Dto::EAM::CreateUserGroupRequest request;
        request.name = vm["name"].as<std::string>();
        request.description = vm["description"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eam", "create-user-group", boost::json::value_from(request));

            if (!response.IsSuccess()) {
                reportFailure("create-user-group", response);
                return 1;
            }

            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EamCli::listUserGroups(const std::vector<std::string> &args) const {
        po::options_description desc("eam list user groups");
        desc.add_options()
                ("prefix,p", po::value<std::string>(), "user group name prefix")
                ("page-size,s", po::value<long>()->default_value(-1), "page size")
                ("page-index,i", po::value<long>()->default_value(-1), "page index")
                ("sort-column,c", po::value<std::string>()->default_value("name"), "sort column")
                ("sort-direction,d", po::value<std::string>()->default_value("asc"), "sort direction");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eam", "list-user-groups", "[--prefix <prefix>] [--page-size <n>] [--page-index <n>] [--sort-column <column>] [--sort-direction <direction>]",
                                   "Lists user groups, optionally filtered by name prefix and paginated. Paginated: page-size defaults to 10, page-index to 0, sort-column to \"created\""
                                   "and sort-direction to \"asc\".",
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

        Dto::EAM::ListUserGroupsRequest request;
        request.pageSize = vm["page-size"].as<long>();
        request.pageIndex = vm["page-index"].as<long>();
        request.sortColumn = vm["sort-column"].as<std::string>();
        request.sortDirection = vm["sort-direction"].as<std::string>();
        if (vm.contains("prefix")) {
            request.prefix = vm["prefix"].as<std::string>();
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eam", "list-user-groups", boost::json::value_from(request));

            if (!response.IsSuccess()) {
                reportFailure("list-user-groups", response);
                return 1;
            }

            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EamCli::addUserToUserGroup(const std::vector<std::string> &args) const {
        po::options_description desc("eam add user to user group options");
        desc.add_options()
                ("user-group,g", po::value<std::string>()->required(), "user group ERN")
                ("user,u", po::value<std::string>()->required(), "user ERN");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eam", "user-group-add-user", "--user-group <ERN> --user <ERN>",
                                   "Adds a user to a user group. User group ERN and user ERN are required.",
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

        Dto::EAM::UserGroupAddUserRequest request;
        request.user = vm["user"].as<std::string>();
        request.userGroup = vm["user-group"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eam", "user-group-add-user", boost::json::value_from(request));

            if (!response.IsSuccess()) {
                reportFailure("user-group-add-user", response);
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EamCli::removeUserFromUserGroup(const std::vector<std::string> &args) const {
        po::options_description desc("eam remove user from a user group options");
        desc.add_options()
                ("user-group,g", po::value<std::string>()->required(), "user group ERN")
                ("user,u", po::value<std::string>()->required(), "user ERN");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eam", "user-group-remove-user", "--user-group <ERN> --user <ERN>",
                                   "Removes a user from a user group. User group ERN and user ERN are required.",
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

        Dto::EAM::UserGroupRemoveUserRequest request;
        request.user = vm["user"].as<std::string>();
        request.userGroup = vm["user-group"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);

            if (const HttpResponse response = client.Post("eam", "user-group-remove-user", boost::json::value_from(request)); !response.IsSuccess()) {
                reportFailure("user-group-remove-user", response);
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EamCli::deleteUserGroup(const std::vector<std::string> &args) const {
        po::options_description desc("eam delete user group options");
        desc.add_options()
                ("name,n", po::value<std::string>()->required(), "group name");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eam", "delete-user-group", "--name <name>",
                                   "Deletes an existing user group. Requires administrator privileges.",
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

        Dto::EAM::DeleteUserGroupRequest request;
        request.name = vm["name"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);

            if (const HttpResponse response = client.Post("eam", "delete-user-group", boost::json::value_from(request)); !response.IsSuccess()) {
                reportFailure("delete-user-group", response);
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EamCli::createAccount(const std::vector<std::string> &args) const {
        po::options_description desc("eam create account options");
        desc.add_options()
                ("account-id,a", po::value<std::string>()->required(), "account ID")
                ("name,n", po::value<std::string>()->required(), "account name")
                ("description,d", po::value<std::string>()->default_value(""), "account description");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eam", "create-account", "--account-id <accountId> --name <name> [--description <description>]",
                                   "Creates a new account. Requires administrator privileges.",
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

        Dto::EAM::CreateAccountRequest request;
        request.accountId = vm["account-id"].as<std::string>();
        request.name = vm["name"].as<std::string>();
        request.description = vm["description"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eam", "create-account", boost::json::value_from(request));

            if (!response.IsSuccess()) {
                reportFailure("create-account", response);
                return 1;
            }

            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EamCli::listAccounts(const std::vector<std::string> &args) const {
        po::options_description desc("eam list accounts");
        desc.add_options()
                ("prefix,p", po::value<std::string>(), "account ID prefix")
                ("page-size,s", po::value<long>()->default_value(-1), "page size")
                ("page-index,i", po::value<long>()->default_value(-1), "page index")
                ("sort-column,c", po::value<std::string>()->default_value("accountId"), "sort column")
                ("sort-direction,d", po::value<std::string>()->default_value("asc"), "sort direction");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eam", "list-accounts", "[--prefix <prefix>] [--page-size <n>] [--page-index <n>] [--sort-column <column>] [--sort-direction <direction>]",
                                   "Lists accounts, optionally filtered by accountId prefix and paginated. Paginated: page-size defaults to 10, page-index to 0, sort-column to \"accountId\""
                                   "and sort-direction to \"asc\".",
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

        Dto::EAM::ListAccountsRequest request;
        request.pageSize = vm["page-size"].as<long>();
        request.pageIndex = vm["page-index"].as<long>();
        request.sortColumn = vm["sort-column"].as<std::string>();
        request.sortDirection = vm["sort-direction"].as<std::string>();
        if (vm.contains("prefix")) {
            request.prefix = vm["prefix"].as<std::string>();
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eam", "list-accounts", boost::json::value_from(request));

            if (!response.IsSuccess()) {
                reportFailure("list-accounts", response);
                return 1;
            }

            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EamCli::deleteAccount(const std::vector<std::string> &args) const {
        po::options_description desc("eam delete account options");
        desc.add_options()
                ("account-id,a", po::value<std::string>()->required(), "account ID");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eam", "delete-account", "--account-id <accountId>",
                                   "Deletes an existing account. Requires administrator privileges, and the "
                                   "account must have no remaining namespaces or user grants.",
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

        Dto::EAM::DeleteAccountRequest request;
        request.accountId = vm["account-id"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);

            if (const HttpResponse response = client.Post("eam", "delete-account", boost::json::value_from(request)); !response.IsSuccess()) {
                reportFailure("delete-account", response);
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EamCli::createNamespace(const std::vector<std::string> &args) const {
        po::options_description desc("eam create namespace options");
        desc.add_options()
                ("account-id,a", po::value<std::string>()->required(), "account ID")
                ("name,n", po::value<std::string>()->required(), "namespace name")
                ("description,d", po::value<std::string>()->default_value(""), "namespace description");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eam", "create-namespace", "--account-id <accountId> --name <name> [--description <description>]",
                                   "Creates a new namespace under an account. Requires administrator privileges "
                                   "on that account.",
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

        Dto::EAM::CreateNamespaceRequest request;
        request.accountId = vm["account-id"].as<std::string>();
        request.name = vm["name"].as<std::string>();
        request.description = vm["description"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eam", "create-namespace", boost::json::value_from(request));

            if (!response.IsSuccess()) {
                reportFailure("create-namespace", response);
                return 1;
            }

            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EamCli::listNamespaces(const std::vector<std::string> &args) const {
        po::options_description desc("eam list namespaces");
        desc.add_options()
                ("account,a", po::value<std::string>()->required(), "account ID")
                ("prefix,p", po::value<std::string>(), "namespace name prefix")
                ("page-size,s", po::value<long>()->default_value(-1), "page size")
                ("page-index,i", po::value<long>()->default_value(-1), "page index")
                ("sort-column,c", po::value<std::string>()->default_value("name"), "sort column")
                ("sort-direction,d", po::value<std::string>()->default_value("asc"), "sort direction");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eam", "list-namespaces", "--account <accountId> [--prefix <prefix>] [--page-size <n>] [--page-index <n>] [--sort-column <column>] [--sort-direction <direction>]",
                                   "Lists namespaces under an account, optionally filtered by name prefix and paginated. Paginated: page-size defaults to 10, page-index to 0, sort-column to \"created\""
                                   "and sort-direction to \"asc\".",
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

        Dto::EAM::ListNamespacesRequest request;
        request.accountId = vm["account"].as<std::string>();
        request.pageSize = vm["page-size"].as<long>();
        request.pageIndex = vm["page-index"].as<long>();
        request.sortColumn = vm["sort-column"].as<std::string>();
        request.sortDirection = vm["sort-direction"].as<std::string>();
        if (vm.contains("prefix")) {
            request.prefix = vm["prefix"].as<std::string>();
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eam", "list-namespaces", boost::json::value_from(request));

            if (!response.IsSuccess()) {
                reportFailure("list-namespaces", response);
                return 1;
            }

            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EamCli::deleteNamespace(const std::vector<std::string> &args) const {
        po::options_description desc("eam delete namespace options");
        desc.add_options()
                ("account,a", po::value<std::string>()->required(), "account ID")
                ("name,n", po::value<std::string>()->required(), "namespace name");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eam", "delete-namespace", "--account <accountId> --name <name>",
                                   "Deletes an existing namespace. Requires administrator privileges on the "
                                   "account, and the namespace must have no remaining user grants.",
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

        Dto::EAM::DeleteNamespaceRequest request;
        request.accountId = vm["account"].as<std::string>();
        request.name = vm["name"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);

            if (const HttpResponse response = client.Post("eam", "delete-namespace", boost::json::value_from(request)); !response.IsSuccess()) {
                reportFailure("delete-namespace", response);
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EamCli::grantNamespaceAccess(const std::vector<std::string> &args) const {
        po::options_description desc("eam grant namespace access options");
        desc.add_options()
                ("user,u", po::value<std::string>()->required(), "user ERN")
                ("account,a", po::value<std::string>()->required(), "account ID")
                ("namespace,s", po::value<std::string>()->required(), "namespace name");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eam", "grant-namespace-access", "--user <ern> --account <accountId> --namespace <name>",
                                   "Grants a user access to a namespace within an account. Requires "
                                   "administrator privileges on that account.",
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

        Dto::EAM::GrantNamespaceAccessRequest request;
        request.user = vm["user"].as<std::string>();
        request.accountId = vm["account"].as<std::string>();
        request.ns = vm["namespace"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);

            if (const HttpResponse response = client.Post("eam", "grant-namespace-access", boost::json::value_from(request)); !response.IsSuccess()) {
                reportFailure("grant-namespace-access", response);
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EamCli::revokeNamespaceAccess(const std::vector<std::string> &args) const {
        po::options_description desc("eam revoke namespace access options");
        desc.add_options()
                ("user,u", po::value<std::string>()->required(), "user ERN")
                ("account,a", po::value<std::string>()->required(), "account ID")
                ("namespace,s", po::value<std::string>()->required(), "namespace name");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eam", "revoke-namespace-access", "--user <ern> --account <accountId> --namespace <name>",
                                   "Revokes a user's access to a namespace within an account. Requires "
                                   "administrator privileges on that account.",
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

        Dto::EAM::RevokeNamespaceAccessRequest request;
        request.user = vm["user"].as<std::string>();
        request.accountId = vm["account"].as<std::string>();
        request.ns = vm["namespace"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);

            if (const HttpResponse response = client.Post("eam", "revoke-namespace-access", boost::json::value_from(request)); !response.IsSuccess()) {
                reportFailure("revoke-namespace-access", response);
                return 1;
            }
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EamCli::changeNamespace(const std::vector<std::string> &args) const {
        po::options_description desc("eam change namespace options");
        desc.add_options()
                ("namespace,n", po::value<std::string>(), "namespace to make active; if missing or empty string clears it");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eam", "change-namespace", "--namespace <name>",
                                   "Switches the active namespace for this session: validated against the "
                                   "current account (and the caller's namespace grants, unless an account "
                                   "administrator) and, on success, persisted to $HOME/.euclid/credentials. Every "
                                   "namespace-scoped command run afterward is automatically restricted to it, "
                                   "until changed again. Pass an empty string to clear it back to unscoped.",
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

        const auto ns = vm["namespace"].empty() ? "" : vm["namespace"].as<std::string>();

        Dto::EAM::ChangeNamespaceRequest request;
        request.ns = ns;

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);

            if (const HttpResponse response = client.Post("eam", "change-namespace", boost::json::value_from(request)); !response.IsSuccess()) {
                reportFailure("change-namespace", response);
                return 1;
            }

            auto entry = Credentials::Load();
            if (!entry.has_value()) {
                std::cerr << "error: not logged in\n";
                return 1;
            }
            entry->nameSpace = ns;
            Credentials::Save(*entry);

            std::cout << (ns.empty() ? "Namespace cleared.\n" : "Namespace set to '" + ns + "'.\n");
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

}