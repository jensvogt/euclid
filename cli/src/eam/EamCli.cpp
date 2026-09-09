// C++ includes
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <memory>

#ifdef _WIN32
#include <conio.h>
#include <io.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

// Boost includes
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

// Euclid includes
#include <euclid/cli/eam/EamCli.h>
#include <euclid/cli/eam/OneLogin.h>
#include <euclid/core/CryptoUtils.h>
#include <euclid/core/HttpUtils.h>
#include <euclid/core/SamlProvider.h>
#include <euclid/dto/eam/OidcAuthorizeRequest.h>
#include <euclid/dto/eam/OidcAuthorizeResponse.h>
#include <euclid/dto/eam/OidcLoginRequest.h>
#include <euclid/dto/eam/SamlAuthorizeResponse.h>

namespace Euclid::CLI {

    namespace po = boost::program_options;
    namespace net = boost::asio;
    namespace beast = boost::beast;
    namespace beast_http = boost::beast::http;
    using tcp = net::ip::tcp;

    namespace {

        // How long the browser has to come back before the login is given up on. Long enough for
        // a provider that asks for a second factor, short enough that a forgotten terminal does
        // not sit on a listening socket all day.
        constexpr auto kCallbackTimeout = std::chrono::minutes(3);

        // What the person sees in the tab the provider redirected, once the code has been caught.
        constexpr auto kCallbackPage =
                "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><title>euclid</title></head>"
                "<body style=\"font-family:sans-serif;padding:3rem\"><h1>Signed in</h1>"
                "<p>You can close this tab and go back to the terminal.</p></body></html>";

        // Catches the one redirect the provider sends back, on a port this process owns for the
        // length of one login.
        //
        // A loopback listener rather than a URL on the euclid gateway because the CLI is a public
        // client: it has no secret to identify itself with, so the code has to come back to the
        // machine that asked for it, and 127.0.0.1 is the one address that cannot be reached from
        // anywhere else. The port is whatever the kernel hands out - it is registered with the
        // provider as a wildcard loopback URI, which is what providers expect of a native client.
        class CallbackListener {

        public:

            CallbackListener() : _acceptor(_ioc, tcp::endpoint(net::ip::make_address("127.0.0.1"), 0)) {}

            [[nodiscard]]
            unsigned short port() const { return _acceptor.local_endpoint().port(); }

            // What the browser eventually brings back: an OIDC code, a SAML session, or the
            // provider's refusal.
            struct Callback {
                std::string code;
                std::string state;
                std::string session;
                std::string error;
            };

            // Waits for a request that carries one of those, answering anything else (a browser
            // asking for the favicon, most often) with a 404 and going back to waiting.
            //
            // @return true if a callback arrived within the timeout.
            bool wait(Callback &callback) {

                bool finished = false;

                // Re-arms itself on every request that turns out not to be the callback, so one
                // stray request does not consume the login.
                std::function<void()> accept = [&] {
                    _acceptor.async_accept([&](const beast::error_code &acceptEc, tcp::socket socket) {
                        if (acceptEc) return;

                        auto stream = std::make_shared<beast::tcp_stream>(std::move(socket));
                        auto buffer = std::make_shared<beast::flat_buffer>();
                        auto request = std::make_shared<beast_http::request<beast_http::string_body> >();

                        beast_http::async_read(*stream, *buffer, *request,
                                               [&, stream, buffer, request](const beast::error_code &readEc, std::size_t) {
                                                   if (readEc) {
                                                       accept();
                                                       return;
                                                   }

                                                   auto parameters = Core::ParseQueryParameters(request->target());

                                                   // A SAML login arrives as a form post from the
                                                   // page euclid answered the assertion with, and
                                                   // carries the whole session in its body.
                                                   if (request->method() == beast_http::verb::post) {
                                                       for (auto &[name, value]: Core::ParseQueryParameters("?" + request->body())) {
                                                           parameters[name] = value;
                                                       }
                                                   }

                                                   const auto hasCode = parameters.contains("code");
                                                   const auto hasSession = parameters.contains("session");
                                                   const auto hasError = parameters.contains("error");

                                                   if (!hasCode && !hasSession && !hasError) {
                                                       beast_http::response<beast_http::string_body> notFound{beast_http::status::not_found, request->version()};
                                                       notFound.prepare_payload();
                                                       beast::error_code ignored;
                                                       beast_http::write(*stream, notFound, ignored);
                                                       accept();
                                                       return;
                                                   }

                                                   if (hasCode) callback.code = parameters.at("code");
                                                   if (hasSession) {
                                                       try {
                                                           callback.session = Core::CryptoUtils::Base64UrlDecode(parameters.at("session"));
                                                       } catch (const std::exception &e) {
                                                           callback.error = std::string("the session euclid sent back could not be read: ") + e.what();
                                                       }
                                                   }
                                                   if (hasError) {
                                                       callback.error = parameters.at("error");
                                                       if (const auto description = parameters.find("error_description"); description != parameters.end()) {
                                                           callback.error += " (" + description->second + ")";
                                                       }
                                                   }
                                                   if (const auto it = parameters.find("state"); it != parameters.end()) callback.state = it->second;

                                                   beast_http::response<beast_http::string_body> page{beast_http::status::ok, request->version()};
                                                   page.set(beast_http::field::content_type, "text/html; charset=utf-8");
                                                   page.body() = kCallbackPage;
                                                   page.prepare_payload();
                                                   beast::error_code writeEc;
                                                   beast_http::write(*stream, page, writeEc);

                                                   finished = true;
                                               });
                    });
                };

                accept();

                const auto deadline = std::chrono::steady_clock::now() + kCallbackTimeout;
                while (!finished && std::chrono::steady_clock::now() < deadline) {
                    _ioc.restart();
                    _ioc.run_for(std::chrono::milliseconds(500));
                }
                return finished;
            }

        private:

            net::io_context _ioc;
            tcp::acceptor _acceptor;
        };

        // Best effort, and said out loud either way: the URL is always printed, so a person on a
        // machine with no browser - over ssh, in a container - can copy it somewhere that has one.
        void openBrowser(const std::string &url) {

            // Single-quoted for the shell, with any quote in the URL closed and reopened around an
            // escaped one. Nothing here comes from a stranger, but a URL is exactly the kind of
            // string that ends up somewhere it was not expected.
            std::string quoted = "'";
            for (const char c: url) {
                if (c == '\'') quoted += "'\\''";
                else quoted += c;
            }
            quoted += "'";

#ifdef _WIN32
            const std::string command = "start \"\" " + quoted;
#elif defined(__APPLE__)
            const std::string command = "open " + quoted + " >/dev/null 2>&1";
#else
            const std::string command = "xdg-open " + quoted + " >/dev/null 2>&1";
#endif
            std::ignore = std::system(command.c_str());
        }

        // Whether there is somebody to ask. Under a scheduler there is not, and a prompt nobody
        // can answer would hang the job rather than fail it - such a caller supplies what is wanted
        // through the environment instead.
        bool hasTerminal() {
#ifdef _WIN32
            return _isatty(_fileno(stdin)) != 0;
#else
            return isatty(STDIN_FILENO) != 0;
#endif
        }

        // Reads a password from the terminal without echoing it.
        std::string readPassword(const std::string &prompt) {

            if (!hasTerminal()) return {};
            std::cerr << prompt << std::flush;

#ifdef _WIN32

            // _getch() reads the console directly and echoes nothing, so there is no console mode
            // to turn off and back on - and no need for <windows.h>, whose macros this file would
            // rather not have.
            std::string password;
            for (;;) {
                const int typed = _getch();
                if (typed == '\r' || typed == '\n' || typed == EOF) break;
                if (typed == 3) return {};// Ctrl-C
                if (typed == '\b' || typed == 127) {
                    if (!password.empty()) password.pop_back();
                    continue;
                }
                password += static_cast<char>(typed);
            }

#else

            // Echo off for the duration, and back on however this returns.
            termios original{};
            if (tcgetattr(STDIN_FILENO, &original) != 0) return {};
            termios quiet = original;
            quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);

            std::string password;
            std::getline(std::cin, password);

            tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);

#endif

            std::cerr << "\n";
            return password;
        }

        // Asks for something at the terminal, echoing it. Empty when there is no terminal, which
        // is what tells the caller that nobody could be asked.
        std::string readLine(const std::string &prompt) {

            if (!hasTerminal()) return {};

            std::cerr << prompt << std::flush;
            std::string line;
            std::getline(std::cin, line);

            // Codes get pasted with a space in the middle, and read back with a stray carriage
            // return on Windows terminals.
            std::erase_if(line, [](const unsigned char c) { return std::isspace(c) != 0; });
            return line;
        }

        // What both logins do with what came back: store the token and access key, make the
        // namespace active if one was asked for, and print the response.
        //
        // Shared because a federated login is not a different kind of session - see the eam
        // module's issueSession() - and because a difference between the two here would be a
        // difference nobody meant to introduce.
        int storeSession(const std::string &endpoint, const std::string &caCertPath, const bool pretty,
                         const boost::json::value &body, const std::string &nameSpace) {

            const auto loginResponse = boost::json::value_to<Dto::EAM::LoginResponse>(body);
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

            if (!nameSpace.empty()) {
                Dto::EAM::ChangeNamespaceRequest nsRequest;
                nsRequest.ns = nameSpace;

                const HttpClient nsClient(endpoint, entry, caCertPath);
                if (const HttpResponse nsResponse = nsClient.Post("eam", "change-namespace", boost::json::value_from(nsRequest)); !nsResponse.IsSuccess()) {
                    Credentials::Save(entry);
                    std::cerr << "warning: logged in, but setting namespace failed (HTTP " << nsResponse.statusCode << "): " << boost::json::serialize(nsResponse.body) << std::endl;
                    return 1;
                }
                entry.nameSpace = nameSpace;
            }

            Credentials::Save(entry);

            Core::WriteJson(std::cout, body, pretty);
            return 0;
        }

    }// namespace

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
                                           {"change-namespace", "Switch the active namespace for this session"},
                                           {"create-access-key", "Create a SigV4 access key and store it locally"},
                                           {"create-account", "Create a new account"},
                                           {"create-namespace", "Create a new namespace under an account"},
                                           {"create-user-group", "Create a new user group"},
                                           {"delete-access-key", "Delete one of your access keys"},
                                           {"delete-account", "Delete an existing account"},
                                           {"delete-namespace", "Delete an existing namespace"},
                                           {"delete-user", "Delete a user account"},
                                           {"delete-user-group", "Delete an existing user group"},
                                           {"grant-namespace-access", "Grant a user access to a namespace within an account"},
                                           {"list-access-keys", "List your access keys"},
                                           {"list-accounts", "List accounts"},
                                           {"list-namespaces", "List namespaces under an account"},
                                           {"list-user-groups", "List user groups"},
                                           {"list-users", "List user accounts"},
                                           {"login", "Authenticate and store a bearer token and SigV4 access key"},
                                           {"register", "Register a new user account"},
                                           {"revoke-namespace-access", "Revoke a user's access to a namespace within an account"},
                                           {"user-group-add-user", "Add an user to an user group"},
                                           {"user-group-remove-user", "Removes an user to an user group"},
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
                ("user,u", po::value<std::string>(), "username; with --onelogin the OneLogin account to sign in as, and with --oidc/--saml not used at all (the identity provider says who you are)")
                ("password,p", po::value<std::string>(), "password; with --onelogin the OneLogin password (otherwise taken from EUCLID_ONELOGIN_PASSWORD, the configuration, or asked for), and not used at all with --oidc/--saml")
                ("oidc,o", po::bool_switch(), "authenticate through the configured OpenID Connect provider in a browser instead of with a password")
                ("saml", po::bool_switch(), "authenticate through the configured SAML identity provider in a browser instead of with a password")
                ("onelogin", po::bool_switch(), "authenticate through OneLogin's API instead of a browser: a SAML assertion is fetched with your password and one-time code (see euclid.cli.onelogin)")
                ("application", po::value<std::string>(), "with --onelogin: which configured OneLogin application to sign in to, e.g. int or prod")
                ("otp", po::value<std::string>(), "with --onelogin: the one-time code; without it the code is computed from the configured TOTP secret, or asked for")
                ("device", po::value<std::string>(), "with --onelogin: which enrolled second factor to use, by device id or type (e.g. \"Google Authenticator\"); asked for when several are enrolled")
                ("show-assertion", po::bool_switch(), "with --onelogin: print what the assertion says - the issuer, audience and recipient euclid has to be configured with - instead of logging in")
                ("namespace,n", po::value<std::string>(), "namespace to make active for this session");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eam", "login",
                                   "--user <username> --password <password> | --oidc | --saml | --onelogin [--application <name>] [--namespace <name>]",
                                   "Authenticates against the Euclid access module and, on success, stores the "
                                   "returned bearer token locally so it is used automatically to authenticate "
                                   "subsequent commands. Also provisions a SigV4 access key on first login (or "
                                   "reuses the existing one on later logins) and stores it alongside the token, "
                                   "so a separate 'create-access-key' call is not needed for Euclid-service commands. "
                                   "There are four ways to prove who you are, and all of them end in the same session: "
                                   "a euclid user ID and password; --oidc, which authenticates in a browser against "
                                   "the configured OpenID Connect provider; --saml, which does the same over SAML 2.0; "
                                   "and --onelogin, which fetches a SAML assertion from OneLogin's API with your "
                                   "OneLogin password and one-time code, for a login where no browser can be opened - "
                                   "that password may be given with --password, but is better taken from "
                                   "EUCLID_ONELOGIN_PASSWORD, from euclid.cli.onelogin, or from the prompt. Neither "
                                   "--oidc nor --saml asks for or sends any password at all. "
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

        const auto nameSpace = vm.contains("namespace") ? vm["namespace"].as<std::string>() : std::string{};

        // Checked here rather than through po::required(), because which options are required
        // depends on the other option: --user/--password are the whole request in a password
        // login and are meaningless in a federated one.
        const bool oidc = vm["oidc"].as<bool>();
        const bool oneLogin = vm["onelogin"].as<bool>();
        const bool saml = vm["saml"].as<bool>() && !oneLogin;// --saml --onelogin means the API flow

        if (oidc && (saml || oneLogin)) {
            std::cerr << "error: --oidc and --saml are two different identity providers; pick one\n";
            return 1;
        }

        if (oneLogin) {
            // Unlike the browser flows, this one signs a person in directly, so both are meaningful
            // here - the OneLogin account and its password, not a euclid one.
            if (vm.contains("password")) {
                // Said once, not enforced: a password on a command line is visible in the shell
                // history and, while the command runs, in the process list. Whoever passes one
                // anyway has usually decided that already - in a script, or on a machine only they
                // use - and that is their call to make.
                std::cerr << "note: a password on the command line is visible in your shell history and the process list;"
                             " EUCLID_ONELOGIN_PASSWORD avoids both\n";
            }
            return loginWithOneLogin(nameSpace,
                                     vm.contains("application") ? vm["application"].as<std::string>() : std::string{},
                                     vm.contains("user") ? vm["user"].as<std::string>() : std::string{},
                                     vm.contains("otp") ? vm["otp"].as<std::string>() : std::string{},
                                     vm.contains("device") ? vm["device"].as<std::string>() : std::string{},
                                     vm.contains("password") ? vm["password"].as<std::string>() : std::string{},
                                     vm["show-assertion"].as<bool>());
        }

        if (oidc || saml) {
            if (vm.contains("user") || vm.contains("password")) {
                std::cerr << "error: --oidc and --saml authenticate through the identity provider; --user and --password are not used\n";
                return 1;
            }
            return oidc ? loginWithOidc(nameSpace) : loginWithSaml(nameSpace);
        }

        if (!vm.contains("user") || !vm.contains("password")) {
            std::cerr << "error: --user and --password are required, unless --oidc, --saml or --onelogin is given\n\n" << desc << std::endl;
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

            return storeSession(_endpoint, _caCertPath, _pretty, response.body, nameSpace);

        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EamCli::loginWithOidc(const std::string &nameSpace) const {

        try {
            // The listener comes first: the redirect URI has to name the port it ended up on, and
            // the provider is told that URI before it ever sends anybody back to it.
            CallbackListener listener;

            Dto::EAM::OidcAuthorizeRequest authorizeRequest;
            authorizeRequest.redirectUri = "http://127.0.0.1:" + std::to_string(listener.port()) + "/callback";

            const HttpClient client(_endpoint, {}, _caCertPath);
            const HttpResponse authorizeResponse = client.Post("eam", "oidc-authorize", boost::json::value_from(authorizeRequest));
            if (!authorizeResponse.IsSuccess()) {
                std::cerr << "error: could not start the OIDC login (HTTP " << authorizeResponse.statusCode << "): "
                          << boost::json::serialize(authorizeResponse.body) << std::endl;
                return 1;
            }

            const auto authorization = boost::json::value_to<Dto::EAM::OidcAuthorizeResponse>(authorizeResponse.body);
            if (authorization.authorizationUrl.empty()) {
                std::cerr << "error: the server did not say where to authenticate\n";
                return 1;
            }

            // Printed before the browser is opened, and printed whether or not opening it works:
            // on a machine without one - over ssh, in a container - this line is the whole flow.
            std::cerr << "Opening your browser to sign in. If it does not open, go to:\n\n  "
                      << authorization.authorizationUrl << "\n\nWaiting for the callback ...\n";
            openBrowser(authorization.authorizationUrl);

            CallbackListener::Callback callback;
            if (!listener.wait(callback)) {
                std::cerr << "error: no callback from the identity provider within three minutes\n";
                return 1;
            }
            if (!callback.error.empty()) {
                std::cerr << "error: the identity provider refused the login: " << callback.error << std::endl;
                return 1;
            }

            // The state that comes back has to be the one that went out. EAM checks this too -
            // it cannot open a state it did not seal - but a mismatch caught here says plainly
            // that the callback belongs to a different login attempt.
            if (callback.state != authorization.state) {
                std::cerr << "error: the callback does not belong to this login attempt\n";
                return 1;
            }

            Dto::EAM::OidcLoginRequest loginRequest;
            loginRequest.code = callback.code;
            loginRequest.state = callback.state;

            const HttpResponse response = client.Post("eam", "oidc-login", boost::json::value_from(loginRequest));
            if (!response.IsSuccess()) {
                std::cerr << "error: login failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }

            return storeSession(_endpoint, _caCertPath, _pretty, response.body, nameSpace);

        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EamCli::loginWithSaml(const std::string &nameSpace) const {

        try {
            CallbackListener listener;

            // Where euclid should send the browser once the assertion has been accepted. Unlike
            // the OIDC flow, this is not a redirect URI the identity provider ever sees: the
            // provider posts its assertion to the gateway, and only afterwards does euclid hand
            // the finished session to this listener. Nothing about the CLI has to be registered
            // with the provider.
            boost::json::object request{{"returnTo", "http://127.0.0.1:" + std::to_string(listener.port()) + "/callback"}};

            const HttpClient client(_endpoint, {}, _caCertPath);
            const HttpResponse authorizeResponse = client.Post("eam", "saml-authorize", request);
            if (!authorizeResponse.IsSuccess()) {
                std::cerr << "error: could not start the SAML login (HTTP " << authorizeResponse.statusCode << "): "
                          << boost::json::serialize(authorizeResponse.body) << std::endl;
                return 1;
            }

            const auto authentication = boost::json::value_to<Dto::EAM::SamlAuthorizeResponse>(authorizeResponse.body);
            if (authentication.authenticationUrl.empty()) {
                std::cerr << "error: the server did not say where to authenticate\n";
                return 1;
            }

            std::cerr << "Opening your browser to sign in. If it does not open, go to:\n\n  "
                      << authentication.authenticationUrl << "\n\nWaiting for the callback ...\n";
            openBrowser(authentication.authenticationUrl);

            CallbackListener::Callback callback;
            if (!listener.wait(callback)) {
                std::cerr << "error: no callback from the identity provider within three minutes\n";
                return 1;
            }
            if (!callback.error.empty()) {
                std::cerr << "error: the identity provider refused the login: " << callback.error << std::endl;
                return 1;
            }
            if (callback.session.empty()) {
                std::cerr << "error: the callback carried no session\n";
                return 1;
            }

            boost::system::error_code ec;
            const auto session = boost::json::parse(callback.session, ec);
            if (ec) {
                std::cerr << "error: the session euclid sent back is not valid JSON\n";
                return 1;
            }

            return storeSession(_endpoint, _caCertPath, _pretty, session, nameSpace);

        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EamCli::loginWithOneLogin(const std::string &nameSpace, const std::string &application,
                                  const std::string &user, const std::string &oneTimeCode,
                                  const std::string &device, const std::string &givenPassword,
                                  const bool showAssertion) const {

        try {
            auto config = OneLoginConfiguration::Read();
            if (!application.empty()) config.application = application;
            if (!user.empty()) config.user = user;
            if (!device.empty()) config.device = device;

            // What was typed on the command line beats what the environment and the file say: it
            // is the most explicit thing the person did.
            if (!givenPassword.empty()) config.password = givenPassword;

            if (const auto problems = config.Validate(); !problems.empty()) {
                std::cerr << "error: OneLogin is not configured for this:\n";
                for (const auto &problem: problems) std::cerr << "  - " << problem << "\n";
                std::cerr << "\nSet euclid.cli.onelogin in the configuration file, or the matching EUCLID_ONELOGIN_* variables.\n";
                return 1;
            }

            auto password = config.password;
            if (password.empty()) password = readPassword("OneLogin password for " + config.user + ": ");
            if (password.empty()) {
                std::cerr << "error: no password: set EUCLID_ONELOGIN_PASSWORD, put it in euclid.cli.onelogin.password, or run this where a terminal can ask for it\n";
                return 1;
            }

            std::cerr << "Asking OneLogin for a SAML assertion ...\n";
            const OneLoginClient client(config, _caCertPath);

            // Asked for only if OneLogin actually wants one, and only if it could not be computed:
            // see OneLoginClient::OneTimeCodeProvider.
            const auto assertion = client.SamlAssertion(
                    password, oneTimeCode,
                    [](const std::string &deviceType) {
                        return readLine(deviceType.empty() ? "OneLogin one-time code: "
                                                           : "OneLogin one-time code (" + deviceType + "): ");
                    },
                    [](const std::vector<OneLoginClient::Device> &devices) -> std::size_t {
                        // Listed rather than guessed at. Which of somebody's authenticators comes
                        // back first is OneLogin's business, not theirs, and a code from the wrong
                        // one is refused in a way that never says so.
                        std::cerr << "OneLogin has more than one second factor enrolled:\n";
                        for (std::size_t i = 0; i < devices.size(); ++i) {
                            std::cerr << "  " << i + 1 << ") " << devices[i].type << " (" << devices[i].id << ")\n";
                        }

                        const auto answer = readLine("Which one? [1] ");
                        if (answer.empty()) return 0;

                        try {
                            const auto picked = std::stoul(answer);
                            if (picked >= 1 && picked <= devices.size()) return picked - 1;
                        } catch (const std::exception &) {
                            // Not a number: fall through to the first, which is what the prompt
                            // offered as the default anyway.
                        }
                        return 0;
                    });

            // Setting an installation up is a chicken and egg problem otherwise: euclid refuses an
            // assertion until it is configured for this provider, and what to configure is written
            // in the assertion. So it can be read out instead of posted.
            if (showAssertion) {
                const auto description = Core::SamlResponseVerifier::Describe(Core::CryptoUtils::Base64Decode(assertion));
                if (!description.has_value()) {
                    std::cerr << "error: OneLogin returned something that is not a SAML response\n";
                    return 1;
                }

                std::cout << "What the assertion says (unverified - this is what the document claims):\n\n"
                          << "  euclid.modules.eam.saml.idp-entity-id : " << description->issuer << "\n"
                          << "  euclid.modules.eam.saml.entity-id     : " << description->audience << "\n"
                          << "  euclid.modules.eam.saml.acs-url       : " << description->recipient << "\n\n"
                          << "  subject                               : " << description->nameId << "\n"
                          << "  valid until                           : " << description->notOnOrAfter << "\n"
                          << "  signed                                : " << (description->hasSignature ? "yes" : "no") << "\n";

                if (!description->attributes.empty()) {
                    std::cout << "\n  attributes (for saml.username-attribute / saml.email-attribute):\n";
                    for (const auto &attribute: description->attributes) std::cout << "    " << attribute << "\n";
                }

                std::cout << "\nThe signing certificate is not in the assertion; take it from the application's SSO tab\n"
                             "in OneLogin and point saml.idp-certificate-file at it.\n";
                return 0;
            }

            // Handed to euclid the same way a browser's form post would deliver it, and verified
            // the same way - the signature is what is trusted, not how it arrived.
            const boost::json::object request{{"samlResponse", assertion}, {"relayState", ""}};

            const HttpClient euclid(_endpoint, {}, _caCertPath);
            const HttpResponse response = euclid.Post("eam", "saml-acs", request);
            if (!response.IsSuccess()) {
                std::cerr << "error: euclid refused the assertion (HTTP " << response.statusCode << "): "
                          << boost::json::serialize(response.body) << std::endl;

                // The one refusal worth explaining, because it is a setting rather than a mistake:
                // an assertion fetched this way answers no request euclid made.
                if (boost::json::serialize(response.body).find("Unsolicited logins are not enabled") != std::string::npos) {
                    std::cerr << "\nAn assertion fetched through OneLogin's API is unsolicited by definition - there is no\n"
                                 "login request from euclid for it to answer. Set euclid.modules.eam.saml.allow-idp-initiated\n"
                                 "to true on the server to accept these.\n";
                }
                return 1;
            }

            return storeSession(_endpoint, _caCertPath, _pretty, response.body, nameSpace);

        } catch (const OneLoginError &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
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