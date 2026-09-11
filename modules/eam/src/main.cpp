// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <algorithm>
#include <filesystem>
#include <iostream>

// Boost includes
#include <boost/program_options.hpp>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/OidcClient.h>
#include <euclid/core/SamlProvider.h>
#include <euclid/core/Version.h>
#include <euclid/core/monitoring/MetricsPusher.h>
#include <euclid/database/RepositoryFactory.h>
#include <EamServer.h>

#define DEFAULT_LOG_LEVEL          "info"
#ifdef _WIN32
#define DEFAULT_CONFIGURATION_FILE "C:\\Program Files\\euclid\\etc\\euclid.json"
#define DEFAULT_SOCKET_PATH        "C:\\Program Files\\euclid\\data\\run\\euclid-eam.sock"
#else
#define DEFAULT_CONFIGURATION_FILE "/usr/local/euclid/etc/euclid.json"
#define DEFAULT_SOCKET_PATH        "/var/run/euclid/euclid-eam.sock"
#endif

namespace po = boost::program_options;

namespace {
    struct CliOptions {
        std::string socketPath;
        std::string configFile;
        std::string logLevel;
        bool consoleLog{true};
        bool fileLog{false};
    };
}

static std::optional<CliOptions> parseCommandLine(int argc, char *argv[]) {

    CliOptions opts;

    po::options_description general("General options", 120, 50);
    general.add_options()
            ("help,h", "Show this help message")
            ("version,v", "Show version information")
            ("config,c", po::value<std::string>(&opts.configFile)->default_value(DEFAULT_CONFIGURATION_FILE), "Path to JSON configuration file")
            ("socket,s", po::value<std::string>(&opts.socketPath)->default_value(DEFAULT_SOCKET_PATH), "Unix domain socket path");

    po::options_description logging("Logging options", 120, 50);
    logging.add_options()
            ("loglevel,l", po::value<std::string>(&opts.logLevel)->default_value(DEFAULT_LOG_LEVEL), "Log level (trace|debug|info|warning|error|fatal)")
            ("console-log", po::value<bool>(&opts.consoleLog)->default_value(true)->implicit_value(true), "Enable console logging")
            ("file-log", po::value<bool>(&opts.fileLog)->default_value(false)->implicit_value(true), "Enable file logging");

    po::options_description all("euclid-eam options");
    all.add(general).add(logging);

    try {
        po::variables_map vm;
        po::store(po::command_line_parser(argc, argv).options(all).run(), vm);

        if (vm.contains("help")) {
            std::cout << "EAM v" << APP_VERSION << " - EAM service process\n\n" << all << "\n";
            return std::nullopt;
        }

        if (vm.contains("version")) {
            std::cout << "EAM version " << APP_VERSION << "\n";
            return std::nullopt;
        }

        po::notify(vm);

    } catch (const po::error &e) {
        std::cerr << "Command line error: " << e.what() << "\n";
        std::cerr << "Use --help for usage information.\n";
        return std::nullopt;
    }

    return opts;
}

static int initializeDatabase(const Euclid::Core::Configuration &cfg) {

    try {

        // Choose backend from config
        const auto backend = cfg.getOr<std::string>("euclid.database.backend", "mongodb");
        if (backend == "memory") {
            log_debug << "Using in-memory database";
            Euclid::Database::RepositoryFactory::instance().initialize(Euclid::Database::BackendType::MEMORY);
        } else if (backend == "emd") {
            // The store the EMD module holds, shared by every process - see modules/emd.
            Euclid::Database::RepositoryFactory::instance().initialize(Euclid::Database::BackendType::EMD);
        } else {
            log_debug << "Using MongoDB database";
            Euclid::Database::Database::instance().initialize();
            Euclid::Database::RepositoryFactory::instance().initialize(Euclid::Database::BackendType::MONGODB);
        }

    } catch (std::exception &e) {
        log_error << "Failed to initialize database: " << e.what();
        return 1;
    } catch (...) {
        log_error << "Failed to initialize database: unknown exception type";
        return 1;
    }
    return 0;
}

// Guarantees the user store always has at least one administrator to log in with. Complements
// handleRegister()'s bootstrap path (which promotes the first *registered* user to admin) by
// covering the case where nobody has registered anyone yet - e.g. a fresh install nobody has
// touched. Non-fatal: a failure here shouldn't stop the service from starting.
static void ensureDefaultObjects(const Euclid::Core::Configuration &cfg) {

    try {
        constexpr auto kDefaultAdminUserId = "admin";
        constexpr auto kDefaultAdminUserGroup = Euclid::Database::kEamAdministratorGroupName;
        const auto repo = Euclid::Database::RepositoryFactory::instance().eamRepository();

        std::string accountId;
        if (cfg.has("euclid.account-ids")) {
            if (const auto accountIds = cfg.getArray<std::string>("euclid.account-ids"); !accountIds.empty()) {
                accountId = accountIds.front();
            }
        }

        // Seed an Account/Namespace for the configured accountId so the DB-backed ScopeLookup
        // (see Database::WireScopeLookup) doesn't lock every account-scoped request out of a
        // fresh/upgraded deployment before an operator has created any accounts by hand.
        if (!accountId.empty() && !repo->accountExists(accountId)) {
            Euclid::Database::Entity::EAM::Account account;
            account.accountId = accountId;
            account.name = "Boostrap Account";
            account.ern = Euclid::Core::createEamAccountErn(accountId);
            account.description = "Bootstrapped from euclid.account-ids";
            account.created = account.modified = std::chrono::system_clock::now();
            repo->upsertAccount(account);
        }

        const std::vector<std::string> namespaces = cfg.has("euclid.namespaces") ? cfg.getArray<std::string>("euclid.namespaces") : std::vector<std::string>{};

        if (!accountId.empty()) {
            for (const auto &ns: namespaces) {
                if (repo->namespaceExists(accountId, ns)) continue;
                Euclid::Database::Entity::EAM::Namespace nsEntity;
                nsEntity.accountId = accountId;
                nsEntity.name = ns;
                nsEntity.ern = Euclid::Core::createEamNamespaceErn(accountId, ns);
                nsEntity.description = "Bootstrapped from euclid.namespaces";
                nsEntity.created = nsEntity.modified = std::chrono::system_clock::now();
                repo->upsertNamespace(nsEntity);
            }
        }

        if (!repo->userExists(kDefaultAdminUserId)) {
            constexpr auto kDefaultAdminPassword = "admin";

            // Create user
            Euclid::Database::Entity::EAM::User user;
            user.ern = Euclid::Core::createEamUserErn(accountId, kDefaultAdminUserId);
            user.userId = kDefaultAdminUserId;
            user.password = Euclid::Core::PasswordUtils::Hash(kDefaultAdminPassword);
            user.region = cfg.getOr<std::string>("euclid.region", "eu-central-1");
            user.accountId = accountId;
            user.email = "admin@example.com";
            user.created = std::chrono::system_clock::now();
            user.modified = std::chrono::system_clock::now();
            user = repo->upsertUser(user);
        }

        // The group every administrator check goes through - see Database::IsEamAdmin() - so an
        // installation without it has nobody who can administer anything. Created whether or not
        // there is a user to put in it: the group is what the check looks for, and membership is a
        // separate question that an operator can answer later through the API.
        //
        // Left exactly as it is when it already exists. Its membership is then somebody's decision,
        // and re-adding an administrator that was deliberately removed is not this function's
        // business.
        if (!repo->userGroupExists(kDefaultAdminUserGroup)) {

            Euclid::Database::Entity::EAM::UserGroup userGroup;
            // Named after the group, not after the user that usually ends up in it: the two are
            // different names ("administrator" and "admin"), and an ERN saying the second for a
            // group called the first is wrong wherever anybody reads it.
            userGroup.ern = Euclid::Core::createEamUserGroupErn(accountId, kDefaultAdminUserGroup);
            userGroup.name = kDefaultAdminUserGroup;
            userGroup.description = "Euclid default administrators group";
            userGroup.accountId = accountId;
            userGroup.region = cfg.getOr<std::string>("euclid.region", "eu-central-1");

            // The bootstrap administrator, if there is one. Looked up rather than assumed: this
            // runs on every start, including on an installation whose "admin" user was renamed or
            // removed long ago, and an empty administrators group is a far better outcome there
            // than an exception that abandons the rest of this function.
            if (const auto user = repo->findUserByUserId(kDefaultAdminUserId); user.has_value()) {
                userGroup.userIds.push_back(user->userId);
            }

            userGroup.created = std::chrono::system_clock::now();
            userGroup.modified = std::chrono::system_clock::now();
            userGroup = repo->upsertUserGroup(userGroup);

            log_warning << "No administrator user group existed; created it (name: '" << userGroup.name
                        << "', region: '" << userGroup.region << "', accountId: '" << accountId
                        << "', members: " << userGroup.userIds.size() << ")";
        }

        // Grant the bootstrap admin an explicit record too - membership in the administrator
        // group (granted above) already bypasses WireGrantLookup's per-user check globally, but
        // this keeps accountGrants inspectable and consistent with how every other user's access
        // is represented.
        if (auto adminUser = repo->findUserByUserId(kDefaultAdminUserId); adminUser.has_value() && !accountId.empty()) {
            if (!std::ranges::any_of(adminUser->accountGrants, [&](const auto &g) { return g.accountId == accountId; })) {
                adminUser->accountGrants.push_back({.accountId = accountId, .namespaces = namespaces, .isAdmin = true, .granted = Euclid::Core::DateTimeUtils::ToISO8601(std::chrono::system_clock::now())});
                repo->upsertUser(*adminUser);
            }
        }

    } catch (const std::exception &e) {
        log_error << "Failed to ensure default objects: " << e.what();
    }
}

int main(const int argc, char *argv[]) {

    const auto cliOpts = parseCommandLine(argc, argv);
    if (!cliOpts) return 0;

    auto &cfg = Euclid::Core::Configuration::instance();

    if (std::filesystem::exists(cliOpts->configFile)) {
        try {
            cfg.load(std::filesystem::path(cliOpts->configFile));
        } catch (const std::exception &e) {
            std::cerr << "Failed to load config: " << e.what() << "\n";
            return 1;
        }
    }

    cfg.set<bool>("euclid.logging.console-active", cliOpts->consoleLog);
    cfg.set<bool>("euclid.logging.file-active", cliOpts->fileLog);

    // ── Initialize logging ──────────────────────────────
    Euclid::Core::LogStream::Initialize();
    // The channel this process's own records carry, so a log gathered from several of
    // them still says which one each line came from - and so this module can be turned
    // down on its own through euclid.logging.channels.
    Euclid::Core::LogStream::SetProcessChannel("eam");
    Euclid::Core::LogStream::ApplyConfiguration(cliOpts->logLevel);

    // ── Validate JWT signing secret ─────────────────────
    if (!Euclid::EAM::EamServer::ValidateJwtSecret()) return 1;

    // ── Report the OIDC configuration ───────────────────
    // Said at startup rather than discovered at the first login attempt, because a configuration
    // mistake here is otherwise invisible until somebody tries to sign in and gets a 500. Not
    // fatal: password login is unaffected by any of it, and refusing to start would turn a
    // federation that was configured wrongly into an installation nobody can log into at all.
    if (const auto oidc = Euclid::Core::OidcConfiguration::FromConfiguration("eam"); oidc.enabled) {
        const auto problems = oidc.Validate();
        for (const auto &problem: problems) log_error << "OIDC login is enabled but not usable: " << problem;
        if (problems.empty()) {
            log_info << "OIDC login enabled, issuer: " << oidc.issuer << ", clientId: " << oidc.clientId
                     << ", jitProvisioning: " << std::boolalpha << oidc.jitProvisioning;
        }
    }

    // ── Report the SAML configuration ───────────────────
    if (const auto saml = Euclid::Core::SamlConfiguration::FromConfiguration("eam"); saml.enabled) {
        const auto problems = saml.Validate();
        for (const auto &problem: problems) log_error << "SAML login is enabled but not usable: " << problem;
        if (problems.empty()) {
            log_info << "SAML login enabled, idpEntityId: " << saml.idpEntityId << ", entityId: " << saml.entityId
                     << ", jitProvisioning: " << std::boolalpha << saml.jitProvisioning
                     << ", idpInitiated: " << saml.allowIdpInitiated;
        }
    }

    // ── Initialize Database ─────────────────────────────
    if (const int error = initializeDatabase(cfg); error != 0) return error;
    Euclid::Database::WireAccessKeyLookup();
    Euclid::Database::WireWorkerThreadsLookup();
    Euclid::Database::WireModuleSocketLookup();
    Euclid::Database::WireScopeLookup();
    Euclid::Database::WireGrantLookup();

    // ── Ensure that some default objects exist ──────────
    ensureDefaultObjects(cfg);

    Euclid::Core::Monitoring::MetricsPusher metricsPusher("eam");
    try {
        Euclid::EAM::EamServer server(cliOpts->socketPath,
                                      Euclid::Core::HttpActionServer::ConfiguredWorkerThreads("eam", 2));
        return server.RunUntilSignal();
    } catch (const std::exception &e) {
        log_error << "Failed to start EAM service: " << e.what();
        return 1;
    } catch (...) {
        log_error << "Failed to start EAM service: unknown exception type";
        return 1;
    }
}