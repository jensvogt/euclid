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
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/TtlCache.h>
#include <euclid/core/monitoring/MetricsPusher.h>
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
     * @brief Registers the per-user grant lookup Core::HttpActionServer::Authenticate() needs,
     * backed by RepositoryFactory::eamRepository().
     *
     * core can't depend on database (database depends on core), so this is the glue that closes
     * the loop, same pattern as WireAccessKeyLookup() - call once per process, after
     * RepositoryFactory::initialize(), in every executable whose HttpActionServer-derived server
     * needs to enforce that the authenticated user actually has a grant for the account/namespace
     * it's requesting.
     */
    inline void WireGrantLookup() {
        Core::HttpActionServer::SetGrantLookup([](const std::string &userId, const std::string &accountId, const std::string &ns) -> bool {
            const auto user = UsersByUserId().get(userId, [](const std::string &id) {
                return RepositoryFactory::instance().eamRepository()->findUserByUserId(id);
            });
            if (!user.has_value()) return false;
            if (IsCachedEamAdmin(userId)) return true;// global admin bypass
            for (const auto &grant: user->accountGrants) {
                if (grant.accountId != accountId) continue;
                if (grant.isAdmin || ns.empty()) return true;// account-scoped admin, or account-only request
                if (std::ranges::contains(grant.namespaces, ns)) return true;
            }
            return false;
        });
    }

    /**
     * @brief Registers the per-resource lookup Core::HttpActionServer::IsResourceAllowed() uses,
     * backed by RepositoryFactory::eamRepository().
     *
     * core can't depend on database (database depends on core), so this is the glue that closes
     * the loop, same pattern as WireGrantLookup() - call once per process, after
     * RepositoryFactory::initialize(), in every module that checks which bucket or queue a
     * caller may act on.
     */
    inline void WireResourceLookup() {
        Core::HttpActionServer::SetResourceLookup([](const std::string &userId, const std::string &resourceErn) -> bool {
            const auto user = UsersByUserId().get(userId, [](const std::string &id) {
                return RepositoryFactory::instance().eamRepository()->findUserByUserId(id);
            });
            if (!user.has_value()) return false;
            // No list means no restriction: humans, and every user written before resource grants
            // existed, are unaffected. A principal that names resources is held to exactly them.
            if (user->resourceGrants.empty()) return true;
            return std::ranges::contains(user->resourceGrants, resourceErn);
        });
    }

    /**
     * @brief Registers the worker-thread lookup Core::HttpActionServer::ConfiguredWorkerThreads()
     * consults, backed by RepositoryFactory::emmRepository().
     *
     * core can't depend on database (database depends on core), so this is the glue that closes
     * the loop, same pattern as WireResourceLookup() - call once per process, after
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

}// namespace Euclid::Database