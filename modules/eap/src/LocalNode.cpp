// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

#ifndef _WIN32
#include <grp.h>
#include <unistd.h>
#endif

// Euclid includes
#include <LocalNode.h>
#include <euclid/core/ApplicationLaunch.h>
#include <euclid/core/BuiltinRoles.h>
#include <euclid/core/Configuration.h>
#include <euclid/core/CryptoUtils.h>
#include <euclid/core/DateTimeUtils.h>
#include <euclid/core/ErnUtils.h>
#include <euclid/core/LogStream.h>
#include <euclid/database/RepositoryFactory.h>

namespace Euclid::EAP::LocalNode {

    namespace {

        // The principal's name. One per installation: there is one manager, so one local node.
        constexpr auto kUserId = "local-node";

#ifdef _WIN32
        constexpr auto kDefaultDirectory = R"(C:\Program Files\euclid\data\local-node)";
#else
        constexpr auto kDefaultDirectory = "/usr/local/euclid/data/local-node";
#endif

        std::string readFile(const std::filesystem::path &path) {
            std::ifstream in(path, std::ios::binary);
            if (!in) return {};
            std::ostringstream buffer;
            buffer << in.rdbuf();
            return buffer.str();
        }

        // Readable by this account and the worker's group, and by nobody else. Set on the
        // temporary before it is renamed into place, so there is no moment at which the file is
        // readable more widely.
        bool restrict(const std::filesystem::path &path, const bool directory) {
            std::error_code ec;
            const auto mode = directory ? std::filesystem::perms::owner_all | std::filesystem::perms::group_read | std::filesystem::perms::group_exec
                                        : std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::group_read;
            std::filesystem::permissions(path, mode, std::filesystem::perm_options::replace, ec);
#ifndef _WIN32
            const auto group = Core::Configuration::instance().getOr<std::string>("euclid.modules.eap.local-node.group", "euclid-wrk");
            if (const auto *entry = getgrnam(group.c_str()); entry != nullptr) {
                if (chown(path.c_str(), static_cast<uid_t>(-1), entry->gr_gid) != 0) {
                    log_warning << "Could not hand " << path.string() << " to group " << group << ": " << std::strerror(errno)
                                << " - the local worker will not be able to read it. Is this account a member of " << group << "?";
                    return false;
                }
            } else {
                log_warning << "No group " << group << " to hand " << path.string() << " to - is euclid-wrk installed?";
                return false;
            }
#endif
            return !ec;
        }

        // Written only when it differs, beside the target and renamed over it - so a worker reading
        // it never sees half a file, and an unchanged restart touches nothing.
        void writeIfChanged(const std::filesystem::path &path, const std::string &content) {

            // The mode and group are put back even when nothing else changed: the server package
            // hands its whole tree back to the manager's account on every install and upgrade
            // (chown -R), which takes these away from the worker's group along with everything else.
            if (readFile(path) == content) {
                restrict(path, false);
                return;
            }

            const auto temporary = std::filesystem::path(path.string() + ".new");
            {
                std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                if (!out) {
                    log_error << "Could not write the local node's " << path.filename().string() << ", path: " << temporary.string();
                    return;
                }
                out << content;
            }
            restrict(temporary, false);

            std::error_code ec;
            std::filesystem::rename(temporary, path, ec);
            if (ec) {
                log_error << "Could not install the local node's " << path.filename().string() << ": " << ec.message();
                return;
            }
            log_info << "Local node " << path.filename().string() << " written, path: " << path.string();
        }

        // The principal, its key and its grant - each made if it is not there, and left alone if it
        // is. Answers the key the worker is to sign with.
        std::optional<Database::Entity::EAM::User> ensurePrincipal(const std::string &accountId, const std::string &region) {

            const auto repository = Database::RepositoryFactory::instance().eamRepository();
            auto user = repository->findUserByUserId(kUserId);

            if (!user.has_value()) {
                Database::Entity::EAM::User created;
                created.userId = kUserId;
                created.accountId = accountId;
                created.region = region;
                created.ern = Core::createEamUserErn(accountId, kUserId);
                created.created = created.modified = std::chrono::system_clock::now();
                // A technical principal, as EAP makes for an application: no password, no login,
                // and an address under .invalid that can never resolve - see createTechnicalUser.
                created.password = "";
                created.email = std::string(kUserId) + "@euclid.invalid";
                created.loginEnabled = false;
                user = repository->upsertUser(created);
                log_info << "EAP created the local node's principal, userId: " << kUserId << ", account: " << accountId;
            }

            // A key, if it has none that works. An operator who deactivated the one it had has said
            // something, but not "stop the local node" - that is what disabling it is for.
            if (std::ranges::none_of(user->accessKeys, [](const auto &key) { return key.active; })) {
                Database::Entity::EAM::AccessKey key;
                key.accessKeyId = Core::CryptoUtils::GenerateAccessKeyId();
                key.secretAccessKey = Core::CryptoUtils::GenerateSecretAccessKey();
                key.active = true;
                key.created = Core::DateTimeUtils::ToISO8601(Core::DateTimeUtils::UtcDateTimeNow());
                user->accessKeys.push_back(key);
                user->modified = std::chrono::system_clock::now();
                user = repository->upsertUser(*user);
                log_info << "EAP gave the local node an access key, accessKeyId: " << key.accessKeyId;
            }

            const auto role = std::string(Core::BuiltinRoles::Node);
            if (const auto grants = repository->findGrantsByPrincipals({user->ern});
                std::ranges::none_of(grants, [&](const auto &grant) { return grant.role == role && grant.accountId == user->accountId; })) {
                Database::Entity::EAM::Grant grant;
                grant.role = role;
                grant.principal = user->ern;
                grant.accountId = user->accountId;
                grant.namespaces = {"*"};
                // Every bucket: the artifacts of whatever is placed here may live in any of them.
                grant.resources = {"*"};
                grant.granted = std::chrono::system_clock::now();
                grant.grantedBy = "eap";
                std::ignore = repository->addGrant(grant);
                log_info << "EAP granted the local node the " << role << " role in account " << user->accountId;
            }
            return user;
        }

    }// namespace

    boost::json::object WorkerConfiguration(const std::filesystem::path &credentialsPath) {

        const auto &configuration = Core::Configuration::instance();

        // The gateway as reached from this host. Always "localhost" rather than whatever host the
        // gateway is configured to bind: it may bind every interface, and the certificate a fresh
        // installation ships is issued to localhost.
        const bool tls = configuration.getOr<bool>("euclid.gateway.tls.enabled", true);
        const auto port = configuration.getOr<long>("euclid.gateway.http.port", 5566);

        boost::json::object worker{
                {"endpoint", std::string(tls ? "https" : "http") + "://localhost:" + std::to_string(port)},
                {"credentials", credentialsPath.string()},
                // The gateway routes to an instance's port at this address, and it is on this host.
                {"address", "127.0.0.1"},
                {"node", configuration.getOr<std::string>("euclid.modules.eap.local-node.name", "local")},
                // What an application asks for to be placed here and nowhere else - nodeLabels
                // {"local": "true"} - until every application is (§13.4).
                {"labels", boost::json::object{{"local", "true"}}},
        };

        if (tls) {
            if (const auto certificate = configuration.getOr<std::string>("euclid.gateway.tls.cert-file", ""); !certificate.empty()) {
                worker["ca-cert"] = certificate;
            }
        }

        // A range of its own, not the manager's: until every application runs on a worker, the
        // manager goes on handing its own instances ports from euclid.modules.eap.http-port-*, and
        // two allocators sharing one range on one host would hand out the same port twice.
        if (const auto first = configuration.getOr<long>("euclid.modules.eap.local-node.http-port-min", 0); first > 0) {
            worker["http-port-min"] = first;
            worker["http-port-max"] = configuration.getOr<long>("euclid.modules.eap.local-node.http-port-max", first);
        }

        // This host's interpreters, as the manager already knows them.
        boost::json::object runtimes;
        for (const auto *key: {"java", "java21", "java25", "python", "nodejs"}) {
            const auto setting = std::string("euclid.modules.eap.runtimes.") + key;
            if (configuration.has(setting)) runtimes[key] = configuration.getOr<std::string>(setting, "");
        }
        if (!runtimes.empty()) worker["runtimes"] = runtimes;

        return boost::json::object{
                {"euclid", boost::json::object{
                                   {"worker", worker},
                                   {"logging", boost::json::object{
                                                       {"level", configuration.getOr<std::string>("euclid.logging.level", "info")},
                                                       // Inherited rather than decided here: a manager that writes a log
                                                       // file has a local node that writes one too, and one that logs only
                                                       // to the journal has a node that does the same.
                                                       {"file-active", configuration.getOr<bool>("euclid.logging.file-active", false)},
                                                       {"dir", configuration.getOr<std::string>("euclid.modules.eap.local-node.log-dir", "/var/lib/euclid-wrk/log")},
                                                       {"prefix", "euclid-wrk"}}}}}};
    }

    void Provision() {

        const auto &configuration = Core::Configuration::instance();
        if (!configuration.getOr<bool>("euclid.modules.eap.local-node.enabled", false)) return;

        try {
            std::string accountId;
            if (configuration.has("euclid.account-ids")) {
                if (const auto ids = configuration.getArray<std::string>("euclid.account-ids"); !ids.empty()) accountId = ids.front();
            }
            if (accountId.empty()) {
                log_warning << "Local node not provisioned: euclid.account-ids names no account to put it in";
                return;
            }

            const auto user = ensurePrincipal(accountId, configuration.getOr<std::string>("euclid.region", "eu-central-1"));
            const auto key = std::ranges::find_if(user->accessKeys, [](const auto &candidate) { return candidate.active; });
            if (key == user->accessKeys.end()) {
                log_error << "Local node not provisioned: its principal has no active access key";
                return;
            }

            const std::filesystem::path directory = configuration.getOr<std::string>("euclid.modules.eap.local-node.dir", kDefaultDirectory);
            std::error_code ec;
            std::filesystem::create_directories(directory, ec);
            restrict(directory, true);

            const auto credentialsPath = directory / "credentials";

            // In the shape euclid-cli writes after a login - see CLI::Credentials - with no token:
            // a worker signs everything it sends with the key, and EAM's own actions, the only ones
            // that need a bearer token, are not among them.
            const boost::json::object credentials{
                    {"token", ""},
                    {"userId", user->userId},
                    {"accountId", user->accountId},
                    {"region", user->region},
                    {"accessKeyId", key->accessKeyId},
                    {"secretAccessKey", key->secretAccessKey},
                    {"isAdmin", false},
                    {"namespace", ""},
            };
            writeIfChanged(credentialsPath, boost::json::serialize(credentials));
            writeIfChanged(directory / "euclid-wrk.json", boost::json::serialize(WorkerConfiguration(credentialsPath)));

        } catch (const std::exception &e) {
            // Not a reason for EAP not to start: everything else it does works without a local node.
            log_error << "Local node not provisioned: " << e.what();
        }
    }

}// namespace Euclid::EAP::LocalNode
