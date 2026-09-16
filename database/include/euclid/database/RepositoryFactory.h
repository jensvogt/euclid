// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 5/24/26.
//

#pragma once

// C++ includes
#include <algorithm>
#include <chrono>
#include <memory>

// Euclid includes
#include <euclid/core/BuiltinRoles.h>
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/TtlCache.h>
#include <euclid/core/monitoring/MetricsPusher.h>
#include <euclid/database/Authorization.h>
#include <euclid/database/EventBus.h>
#include <euclid/database/repository/eam/IEamRepository.h>
#include <euclid/database/repository/ekv/IEkvRepository.h>
#include <euclid/database/repository/ekv/MongoEkvRepository.h>
#include <euclid/database/repository/eam/MongoEamRepository.h>
#include <euclid/database/repository/ekm/IEkmRepository.h>
#include <euclid/database/repository/ekm/MongoEkmRepository.h>
#include <euclid/database/repository/emm/IEmmRepository.h>
#include <euclid/database/repository/emm/MongoEmmRepository.h>
#include <euclid/database/repository/emo/IEmoRepository.h>
#include <euclid/database/repository/emo/MongoEmoRepository.h>
#include <euclid/database/repository/ens/IEnsRepository.h>
#include <euclid/database/AuditWriter.h>
#include <euclid/database/repository/ead/MongoEadRepository.h>
#include <euclid/database/repository/ens/MongoEnsRepository.h>
#include <euclid/database/repository/eqs/IEqsRepository.h>
#include <euclid/database/repository/eqs/MongoEqsRepository.h>
#include <euclid/database/repository/esm/IEsmRepository.h>
#include <euclid/database/repository/ess/IEssRepository.h>
#include <euclid/database/repository/ess/MongoEssRepository.h>
#include <euclid/database/repository/eap/MongoEapRepository.h>
#include <euclid/database/repository/eag/MongoEagRepository.h>
#include <euclid/database/repository/ets/MongoEtsRepository.h>
#include <euclid/database/repository/esm/MongoEsmRepository.h>

namespace Euclid::Database {

    enum class BackendType {
        /**
         * @brief MongoDB, which every repository is written against.
         */
        MONGODB,

        /**
         * @brief An in-memory store inside this process - for a test that exercises one module.
         */
        MEMORY,

        /**
         * @brief The in-memory store the EMD module holds, shared by every process.
         */
        EMD
    };

    class RepositoryFactory {

    public:

        static RepositoryFactory &instance() {
            static RepositoryFactory inst;
            return inst;
        }

        void initialize(const BackendType type) {
            _backend = type;
            if (type == BackendType::MEMORY) Database::instance().initializeMemory();
            if (type == BackendType::EMD) {
                Database::instance().initializeRemote(Core::Configuration::instance().getOr<std::string>(
                        "euclid.modules.emd.socketPath", "/var/run/euclid/euclid-emd.sock"));
            }
            warmUp();
        }

        /**
         * @brief Constructs every repository now, rather than on whichever request needs one first.
         *
         * @par
         * A repository is built on first use and ensures its indexes as it is built. That makes
         * the first request to a freshly started module pay for the setup of every module it
         * touches - and that request belongs to a client, which is holding a timeout open while
         * it happens. It is how a module can have a socket, be routed to, and still not answer
         * within ten seconds.
         *
         * @par
         * Doing it here moves the cost to process startup, where the manager is already waiting
         * and nobody else is. Ensuring an index that exists is a single cheap command, so this is
         * a few dozen round trips once, and only for the MongoDB backend - the in-memory one has
         * nothing to set up. Failures are swallowed by the repositories themselves, so a module
         * whose database is not there still starts, exactly as before.
         */
        void warmUp() const {
            if (_backend == BackendType::MEMORY) return;

            // Not for the shared store either, and for a reason worth stating: the manager
            // initializes its database before it starts anything, and the store is one of the
            // things it has not started yet. Warming up would mean eleven repositories each
            // waiting out the connect retry against a process that cannot exist until the manager
            // gets past this line. The repositories are built on first use instead, which for the
            // manager is after EMD is running, and for a module is immediately - it starts after
            // the store it depends on.
            if (_backend == BackendType::EMD) return;

            std::ignore = emmRepository();
            std::ignore = eqsRepository();
            std::ignore = ensRepository();
            std::ignore = eadRepository();
            std::ignore = eamRepository();
            std::ignore = emoRepository();
            std::ignore = esmRepository();
            std::ignore = ekmRepository();
            std::ignore = etsRepository();
            std::ignore = eagRepository();
            std::ignore = eapRepository();
            std::ignore = essRepository();
            std::ignore = ekvRepository();

            // The event bus sets its indexes up the same way, on the first Subscribe or Publish -
            // which for EES is the subscribe-events call of whichever client got there first.
            EventBus::instance().Warm();
        }

        [[nodiscard]]
        std::shared_ptr<IEmmRepository> emmRepository() const {
            static auto repo = createEmmRepository();
            return repo;
        }

        [[nodiscard]]
        std::shared_ptr<IEqsRepository> eqsRepository() const {
            static auto repo = createEqsRepository();
            return repo;
        }

        [[nodiscard]]
        std::shared_ptr<IEnsRepository> ensRepository() const {
            static auto repo = createEnsRepository();
            return repo;
        }

        [[nodiscard]]
        std::shared_ptr<IEadRepository> eadRepository() const {
            static auto repo = createEadRepository();
            return repo;
        }

        [[nodiscard]]
        std::shared_ptr<IEamRepository> eamRepository() const {
            static auto repo = createEamRepository();
            return repo;
        }

        [[nodiscard]]
        std::shared_ptr<IEmoRepository> emoRepository() const {
            static auto repo = createEmoRepository();
            return repo;
        }

        [[nodiscard]]
        std::shared_ptr<IEsmRepository> esmRepository() const {
            static auto repo = createEsmRepository();
            return repo;
        }

        [[nodiscard]]
        std::shared_ptr<IEkmRepository> ekmRepository() const {
            static auto repo = createEkmRepository();
            return repo;
        }

        [[nodiscard]]
        std::shared_ptr<IEssRepository> essRepository() const {
            static auto repo = createEssRepository();
            return repo;
        }

        [[nodiscard]]
        std::shared_ptr<IEkvRepository> ekvRepository() const {
            static auto repo = createEkvRepository();
            return repo;
        }

        [[nodiscard]]
        std::shared_ptr<IEtsRepository> etsRepository() const {
            static auto repo = createEtsRepository();
            return repo;
        }

        [[nodiscard]]
        std::shared_ptr<IEapRepository> eapRepository() const {
            static auto repo = createEapRepository();
            return repo;
        }

        [[nodiscard]]
        std::shared_ptr<IEagRepository> eagRepository() const {
            static auto repo = createEagRepository();
            return repo;
        }

    private:

        BackendType _backend = BackendType::MONGODB;

        [[nodiscard]]
        std::shared_ptr<IEmmRepository> createEmmRepository() const {
            // MongoEmmRepository whatever the backend: it talks to
            // Database::collection(), which is MongoDB, an in-process document store or the
            // store EMD holds - see Emd::DocumentStore. One implementation, so there is no
            // second one to keep in step.
            return std::make_shared<MongoEmmRepository>();
        }

        [[nodiscard]]
        std::shared_ptr<IEqsRepository> createEqsRepository() const {
            // MongoEqsRepository whatever the backend: it talks to
            // Database::collection(), which is MongoDB, an in-process document store or the
            // store EMD holds - see Emd::DocumentStore. One implementation, so there is no
            // second one to keep in step.
            return std::make_shared<MongoEqsRepository>();
        }

        [[nodiscard]]
        std::shared_ptr<IEadRepository> createEadRepository() const {
            // MongoEadRepository whatever the backend, like the rest: it talks to
            // Database::collection(), which is MongoDB, an in-process document store or the
            // store EMD holds.
            return std::make_shared<MongoEadRepository>();
        }

        [[nodiscard]]
        std::shared_ptr<IEnsRepository> createEnsRepository() const {
            // MongoEnsRepository whatever the backend: it talks to
            // Database::collection(), which is MongoDB, an in-process document store or the
            // store EMD holds - see Emd::DocumentStore. One implementation, so there is no
            // second one to keep in step.
            return std::make_shared<MongoEnsRepository>();
        }

        [[nodiscard]]
        std::shared_ptr<IEamRepository> createEamRepository() const {
            // MongoEamRepository whatever the backend: it talks to
            // Database::collection(), which is MongoDB, an in-process document store or the
            // store EMD holds - see Emd::DocumentStore. One implementation, so there is no
            // second one to keep in step.
            return std::make_shared<MongoEamRepository>();
        }

        [[nodiscard]]
        std::shared_ptr<IEmoRepository> createEmoRepository() const {
            // MongoEmoRepository whatever the backend: it talks to
            // Database::collection(), which is MongoDB, an in-process document store or the
            // store EMD holds - see Emd::DocumentStore. One implementation, so there is no
            // second one to keep in step.
            return std::make_shared<MongoEmoRepository>();
        }

        [[nodiscard]]
        std::shared_ptr<IEtsRepository> createEtsRepository() const {
            // MongoEtsRepository whatever the backend: it talks to
            // Database::collection(), which is MongoDB, an in-process document store or the
            // store EMD holds - see Emd::DocumentStore. One implementation, so there is no
            // second one to keep in step.
            return std::make_shared<MongoEtsRepository>();
        }

        [[nodiscard]]
        std::shared_ptr<IEapRepository> createEapRepository() const {
            // MongoEapRepository whatever the backend: it talks to
            // Database::collection(), which is MongoDB, an in-process document store or the
            // store EMD holds - see Emd::DocumentStore. One implementation, so there is no
            // second one to keep in step.
            return std::make_shared<MongoEapRepository>();
        }

        [[nodiscard]]
        std::shared_ptr<IEagRepository> createEagRepository() const {
            // MongoEagRepository whatever the backend: it talks to
            // Database::collection(), which is MongoDB, an in-process document store or the
            // store EMD holds - see Emd::DocumentStore. One implementation, so there is no
            // second one to keep in step.
            return std::make_shared<MongoEagRepository>();
        }

        [[nodiscard]]
        std::shared_ptr<IEsmRepository> createEsmRepository() const {
            // MongoEsmRepository whatever the backend: it talks to
            // Database::collection(), which is MongoDB, an in-process document store or the
            // store EMD holds - see Emd::DocumentStore. One implementation, so there is no
            // second one to keep in step.
            return std::make_shared<MongoEsmRepository>();
        }

        [[nodiscard]]
        std::shared_ptr<IEkmRepository> createEkmRepository() const {
            // MongoEkmRepository whatever the backend: it talks to
            // Database::collection(), which is MongoDB, an in-process document store or the
            // store EMD holds - see Emd::DocumentStore. One implementation, so there is no
            // second one to keep in step.
            return std::make_shared<MongoEkmRepository>();
        }

        [[nodiscard]]
        std::shared_ptr<IEkvRepository> createEkvRepository() const {
            // One implementation whatever the backend, as everywhere else here - see
            // createEssRepository().
            return std::make_shared<MongoEkvRepository>();
        }

        std::shared_ptr<IEssRepository> createEssRepository() const {
            // MongoEssRepository whatever the backend: it talks to
            // Database::collection(), which is MongoDB, an in-process document store or the
            // store EMD holds - see Emd::DocumentStore. One implementation, so there is no
            // second one to keep in step.
            return std::make_shared<MongoEssRepository>();
        }
    };

    /**
     * @brief How long an authentication lookup may reuse what it read last time.
     *
     * Five seconds by default, and {@code euclid.auth.cache-ttl} in milliseconds overrides it;
     * zero turns the caching off. See AuthenticationUsers() for what the number buys and costs.
     */
    inline std::chrono::milliseconds AuthCacheTtl() {
        return std::chrono::milliseconds(Core::Configuration::instance().getOr<int>("euclid.auth.cache-ttl", 5000));
    }

    /**
     * @brief The user documents the authentication lookups read, remembered briefly.
     *
     * Every authenticated request resolves an access key to a user and then that user to their
     * grants, and does it twice - the gateway authenticates the request and the module
     * authenticates it again. That is four reads of the same one or two documents per request,
     * for documents that change when somebody is given a permission, which is to say almost never.
     *
     * Two caches rather than one because the two lookups are keyed differently - by access key id
     * and by user id - and a request generally needs both. They are deliberately keyed on the
     * document read rather than on the answer derived from it, so that the account, namespace,
     * grant and resource checks all get faster without four separate caches to reason about, and
     * so that what can be stale is one thing and easy to state: the user document, for up to the
     * TTL.
     *
     * The staleness is the price and it is an authorisation price - a deactivated key or a revoked
     * grant keeps working for that long. Hence seconds, and hence configurable.
     */
    inline Core::TtlCache<std::string, Entity::EAM::User> &UsersByAccessKeyId() {
        static Core::TtlCache<std::string, Entity::EAM::User> cache(AuthCacheTtl());
        return cache;
    }

    /**
     * @brief The by-user-id half of UsersByAccessKeyId(), used by the grant and resource checks.
     */
    inline Core::TtlCache<std::string, Entity::EAM::User> &UsersByUserId() {
        static Core::TtlCache<std::string, Entity::EAM::User> cache(AuthCacheTtl());
        return cache;
    }

    /**
     * @brief The administrator group, remembered on the same terms as the user documents.
     *
     * Read on every grant check that reaches the admin bypass, and the group that decides who
     * administers an installation is not one that changes during a request.
     */
    inline Core::TtlCache<std::string, Entity::EAM::UserGroup> &AdministratorGroup() {
        static Core::TtlCache<std::string, Entity::EAM::UserGroup> cache(AuthCacheTtl());
        return cache;
    }

    /**
     * @brief IsEamAdmin() against the cached administrator group.
     */
    inline bool IsCachedEamAdmin(const std::string &userId) {
        const auto group = AdministratorGroup().get(kEamAdministratorGroupName, [](const std::string &name) {
            return RepositoryFactory::instance().eamRepository()->findUserGroupByName(name);
        });
        return group.has_value() && std::ranges::contains(group->userIds, userId);
    }

    /**
     * @brief Registers the SigV4 access-key lookup Core::HttpActionServer::Authenticate() needs,
     * backed by RepositoryFactory::accessRepository().
     *
     * core can't depend on database (database depends on core), so this is the glue that closes
     * the loop - call once per process, after RepositoryFactory::initialize(), in every
     * executable whose HttpActionServer-derived server needs to verify SigV4-signed requests
     * (the gateway and every Euclid-service module).
     */
    inline void WireAccessKeyLookup() {
        Core::HttpActionServer::SetAccessKeyLookup([](const std::string &accessKeyId) -> std::optional<Core::HttpActionServer::AccessKeyRecord> {
            const auto user = UsersByAccessKeyId().get(accessKeyId, [](const std::string &id) {
                return RepositoryFactory::instance().eamRepository()->findUserByAccessKeyId(id);
            });
            if (!user.has_value()) return std::nullopt;
            for (const auto &key: user->accessKeys) {
                if (key.accessKeyId == accessKeyId && key.active) {
                    return Core::HttpActionServer::AccessKeyRecord{.secretAccessKey = key.secretAccessKey, .userId = user->userId};
                }
            }
            return std::nullopt;
        });
    }

    /**
     * @brief Registers the account/namespace scope lookup Core::HttpActionServer::Authenticate()
     * needs, backed by RepositoryFactory::eamRepository().
     *
     * core can't depend on database (database depends on core), so this is the glue that closes
     * the loop, same pattern as WireAccessKeyLookup() - call once per process, after
     * RepositoryFactory::initialize(), in every executable whose HttpActionServer-derived server
     * needs to validate x-euclid-account-id/x-euclid-namespace against the database instead of
     * static config.
     */
    inline void WireScopeLookup() {
        Core::HttpActionServer::SetScopeLookup([](const std::string &accountId, const std::string &ns) -> bool {
            const auto repo = RepositoryFactory::instance().eamRepository();
            if (!repo->accountExists(accountId)) return false;
            return ns.empty() || repo->namespaceExists(accountId, ns);
        });
    }

    /**
     * @brief Registers where Core::HttpActionServer::Dispatch() writes the audit trail, backed by
     * RepositoryFactory::eadRepository().
     *
     * core can't depend on database (database depends on core), so this is the glue that closes
     * the loop, same pattern as WireAccessKeyLookup() - call once per process, after
     * RepositoryFactory::initialize(), in every module whose commands should appear in the trail.
     * A module that never calls this records nothing, which is what the manager and the tools
     * built on HttpActionServer keep doing.
     *
     * @par
     * Queued rather than written here: the sink is called on the request thread, and an insert
     * there would put a database round trip on the critical path of every command euclid answers.
     * See Database::AuditWriter.
     *
     * @par
     * Redaction happens on this side of the boundary, so a module cannot record a raw body by
     * forgetting to ask for it - there is no call that stores one.
     */
    inline void WireAuditSink() {
        Core::HttpActionServer::SetAuditSink([](const Core::HttpActionServer::AuditRecord &record) {
            Entity::EAD::AuditEvent event;
            event.accountId = record.accountId;
            event.nameSpace = record.nameSpace;
            event.userId = record.userId;
            event.moduleName = record.moduleName;
            event.command = record.command;
            event.parameters = Entity::EAD::Redact(record.parameters);
            event.status = record.status;
            event.created = std::chrono::system_clock::now();

            // Zero means keep for ever, and then no expiresAt is written at all - a TTL index
            // ignores a document that has no such field, so turning retention on later sweeps only
            // what was written after it.
            if (const auto days = Core::Configuration::instance().getOr<long>("euclid.modules.ead.retention", 365); days > 0) {
                event.expiresAt = event.created + std::chrono::hours(24 * days);
            }

            AuditWriter::instance().Write(event);
        });
    }

    /**
     * @brief Registers the worker-thread lookup Core::HttpActionServer::ConfiguredWorkerThreads()
     * consults, backed by RepositoryFactory::emmRepository().
     *
     * core can't depend on database (database depends on core), so this is the glue that closes
     * the loop - call once per process, after
     * RepositoryFactory::initialize() and before the module constructs its server, in every module
     * whose thread count "euclid-cli emm set-threads" should be able to change. Modules that never
     * call this keep reading euclid.json alone.
     */
    inline void WireWorkerThreadsLookup() {
        Core::HttpActionServer::SetWorkerThreadsLookup([](const std::string &moduleName) -> int {
            const auto module = RepositoryFactory::instance().emmRepository()->findByName(moduleName);
            // -1 both when nobody has asked for a count and when the module has no document yet,
            // which is the ordinary case the first time a module ever starts.
            return module.has_value() ? module->desiredThreads : -1;
        });
    }

    /**
     * @brief Registers the module-socket lookup Core::Monitoring::MetricsPusher needs to find
     * the monitoring module's live instance(s), backed by RepositoryFactory::moduleRepository().
     *
     * core can't depend on database (database depends on core), so this is the glue that closes
     * the loop, same pattern as WireAccessKeyLookup() - call once per process, after
     * RepositoryFactory::initialize(), in every executable that pushes its own metrics (i.e.
     * constructs a Core::Monitoring::MetricsPusher).
     */
    inline void WireModuleSocketLookup() {
        Core::Monitoring::MetricsPusher::SetModuleSocketLookup([](const std::string &moduleName) -> std::vector<std::string> {
            std::vector<std::string> sockets;
            for (const auto &module: RepositoryFactory::instance().emmRepository()->findAll()) {
                if (module.name != moduleName) continue;
                for (const auto &instance: module.instances) {
                    if (instance.state == Entity::ModuleState::RUNNING) sockets.push_back(instance.socketPath);
                }
            }
            return sockets;
        });
    }


    /**
     * @brief Every ERN a caller's grants can hang off: their own, and each group they belong to.
     *
     * @par
     * Their rights are the union of all of them, so this is one list and one query rather than a
     * loop of lookups. Groups are installation-wide and few, and the set is cached the way the
     * administrator group already is.
     */
    inline std::vector<std::string> PrincipalsOf(const Entity::EAM::User &user) {

        std::vector<std::string> principals{user.ern};

        for (const auto repo = RepositoryFactory::instance().eamRepository();
             const auto &group: repo->listUserGroups("", 0, 0, "name")) {
            if (std::ranges::contains(group.userIds, user.userId)) principals.push_back(group.ern);
        }
        return principals;
    }

    /**
     * @brief The principal euclid's own inter-module traffic acts as.
     *
     * @par
     * EAP starting an application, ESM notifying an ENS topic, the gateway forwarding to a module:
     * those authenticate as the module rather than as a user, and they cross accounts by design.
     * Rather than exempting them from the gate they are named, so the internal path is one entry in
     * a log and in check-permission's answer rather than an unexplained allow. Nothing can create,
     * bind or revoke it - it is a constant, not a row.
     */
    inline constexpr auto kSystemPrincipal = "system";

    /**
     * @brief Registers the authorization lookup Core::HttpActionServer's gate consults.
     *
     * @par
     * The glue that closes the loop core cannot close itself: roles, grants and users live here,
     * and core depends on nothing of this. Call once per process, after initialize(), in every
     * executable whose HttpActionServer-derived server should be gated.
     *
     * @par
     * Answers only the module-and-action half of the question. The resource an action names lives
     * in the request body and only the handler knows which field holds it - see
     * docs/role-concept.md §4.1 for why enforcement is two-layer.
     */
    inline void WireAuthorizationLookup() {

        Core::HttpActionServer::SetAuthorizationLookup(
                [](const boost::beast::http::request<boost::beast::http::string_body> &req,
                   const std::string &target, const std::string &action) -> Core::HttpActionServer::AuthorizationDecision {
                    const auto auth = Core::HttpActionServer::Authenticate(req);

                    // Not authenticated is not the gate's answer to give. The handler's own
                    // authenticate() will say 401, which is the honest status - refusing here would
                    // turn every unauthenticated request into a 403 about permissions it was never
                    // going to be asked for.
                    if (!auth.subject.has_value()) return {.allowed = true, .reason = "not authenticated; left to the handler"};

                    if (*auth.subject == kSystemPrincipal) {
                        return {.allowed = true, .reason = "euclid's own inter-module traffic"};
                    }

                    const auto user = UsersByUserId().get(*auth.subject, [](const std::string &id) {
                        return RepositoryFactory::instance().eamRepository()->findUserByUserId(id);
                    });
                    if (!user.has_value()) return {.allowed = true, .reason = "unknown subject; left to the handler"};

                    if (IsCachedEamAdmin(user->userId)) {
                        return {.allowed = true, .reason = "member of the administrator user group"};
                    }

                    const auto repo = RepositoryFactory::instance().eamRepository();
                    const auto result = Authorization::Allows(
                            {.target = target,
                             .action = action,
                             .accountId = std::string(req["x-euclid-account-id"]),
                             .nameSpace = std::string(req["x-euclid-namespace"]),
                             .resourceErn = {}},
                            repo->findGrantsByPrincipals(PrincipalsOf(*user)),
                            [&repo](const std::string &accountId, const std::string &role) -> std::optional<std::vector<std::string> > {
                                if (const auto stored = repo->findRoleByName(accountId, role)) return stored->permissions;
                                if (Core::BuiltinRoles::Exists(role)) return Core::BuiltinRoles::PermissionsOf(role);
                                return std::nullopt;
                            });

                    return {.allowed = result.allowed, .reason = result.reason};
                });


        // The resource half, asked by a handler once it has read which bucket or queue the request
        // is about. Same grants, same roles - the only difference is that Grant::resources is now
        // consulted, because there is finally something to match it against.
        Core::HttpActionServer::SetResourceAuthorizationLookup(
                [](const boost::beast::http::request<boost::beast::http::string_body> &req,
                   const std::string &target, const std::string &action,
                   const std::string &resourceErn) -> Core::HttpActionServer::AuthorizationDecision {
                    const auto auth = Core::HttpActionServer::Authenticate(req);
                    if (!auth.subject.has_value()) return {.allowed = true, .reason = "not authenticated; left to the handler"};

                    if (*auth.subject == kSystemPrincipal) {
                        return {.allowed = true, .reason = "euclid's own inter-module traffic"};
                    }

                    const auto user = UsersByUserId().get(*auth.subject, [](const std::string &id) {
                        return RepositoryFactory::instance().eamRepository()->findUserByUserId(id);
                    });
                    if (!user.has_value()) return {.allowed = true, .reason = "unknown subject; left to the handler"};

                    if (IsCachedEamAdmin(user->userId)) {
                        return {.allowed = true, .reason = "member of the administrator user group"};
                    }

                    const auto repo = RepositoryFactory::instance().eamRepository();
                    const auto result = Authorization::Allows(
                            {.target = target,
                             .action = action,
                             .accountId = std::string(req["x-euclid-account-id"]),
                             .nameSpace = std::string(req["x-euclid-namespace"]),
                             .resourceErn = resourceErn},
                            repo->findGrantsByPrincipals(PrincipalsOf(*user)),
                            [&repo](const std::string &accountId, const std::string &role) -> std::optional<std::vector<std::string> > {
                                if (const auto stored = repo->findRoleByName(accountId, role)) return stored->permissions;
                                if (Core::BuiltinRoles::Exists(role)) return Core::BuiltinRoles::PermissionsOf(role);
                                return std::nullopt;
                            });

                    return {.allowed = result.allowed, .reason = result.reason};
                });

    }

}// namespace Euclid::Database