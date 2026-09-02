//
// Created by vogje01 on 5/24/26.
//

#pragma once

// C++ includes
#include <algorithm>
#include <memory>

// Euclid includes
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/monitoring/MetricsPusher.h>
#include <euclid/database/EventBus.h>
#include <euclid/database/repository/eam/IEamRepository.h>
#include <euclid/database/repository/eam/MemoryEamRepository.h>
#include <euclid/database/repository/eam/MongoEamRepository.h>
#include <euclid/database/repository/ekm/IEkmRepository.h>
#include <euclid/database/repository/ekm/MemoryEkmRepository.h>
#include <euclid/database/repository/ekm/MongoEkmRepository.h>
#include <euclid/database/repository/emm/IEmmRepository.h>
#include <euclid/database/repository/emm/MemoryEmmRepository.h>
#include <euclid/database/repository/emm/MongoEmmRepository.h>
#include <euclid/database/repository/emo/IEmoRepository.h>
#include <euclid/database/repository/emo/MemoryEmoRepository.h>
#include <euclid/database/repository/emo/MongoEmoRepository.h>
#include <euclid/database/repository/ens/IEnsRepository.h>
#include <euclid/database/repository/ens/MemoryEnsRepository.h>
#include <euclid/database/repository/ens/MongoEnsRepository.h>
#include <euclid/database/repository/eqs/IEqsRepository.h>
#include <euclid/database/repository/eqs/MemoryEqsRepository.h>
#include <euclid/database/repository/eqs/MongoEqsRepository.h>
#include <euclid/database/repository/esm/IEsmRepository.h>
#include <euclid/database/repository/esm/MemoryEsmRepository.h>
#include <euclid/database/repository/eap/MemoryEapRepository.h>
#include <euclid/database/repository/eap/MongoEapRepository.h>
#include <euclid/database/repository/ets/MemoryEtsRepository.h>
#include <euclid/database/repository/ets/MongoEtsRepository.h>
#include <euclid/database/repository/esm/MongoEsmRepository.h>

namespace Euclid::Database {

    enum class BackendType { MONGODB, MEMORY };

    class RepositoryFactory {

    public:

        static RepositoryFactory &instance() {
            static RepositoryFactory inst;
            return inst;
        }

        void initialize(const BackendType type) {
            _backend = type;
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
            if (_backend != BackendType::MONGODB) return;

            std::ignore = emmRepository();
            std::ignore = eqsRepository();
            std::ignore = ensRepository();
            std::ignore = eamRepository();
            std::ignore = emoRepository();
            std::ignore = esmRepository();
            std::ignore = ekmRepository();
            std::ignore = etsRepository();
            std::ignore = eapRepository();

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
        std::shared_ptr<IEtsRepository> etsRepository() const {
            static auto repo = createEtsRepository();
            return repo;
        }

        [[nodiscard]]
        std::shared_ptr<IEapRepository> eapRepository() const {
            static auto repo = createEapRepository();
            return repo;
        }

    private:

        BackendType _backend = BackendType::MONGODB;

        [[nodiscard]]
        std::shared_ptr<IEmmRepository> createEmmRepository() const {
            switch (_backend) {
                case BackendType::MONGODB:
                    return std::make_shared<MongoEmmRepository>();
                case BackendType::MEMORY:
                    return std::make_shared<MemoryEmmRepository>();
            }
            return std::make_shared<MemoryEmmRepository>();
        }

        [[nodiscard]]
        std::shared_ptr<IEqsRepository> createEqsRepository() const {
            switch (_backend) {
                case BackendType::MONGODB:
                    return std::make_shared<MongoEqsRepository>();
                case BackendType::MEMORY:
                    return std::make_shared<MemoryEqsRepository>();
            }
            return std::make_shared<MemoryEqsRepository>();
        }

        [[nodiscard]]
        std::shared_ptr<IEnsRepository> createEnsRepository() const {
            switch (_backend) {
                case BackendType::MONGODB:
                    return std::make_shared<MongoEnsRepository>();
                case BackendType::MEMORY:
                    return std::make_shared<MemoryEnsRepository>();
            }
            return std::make_shared<MemoryEnsRepository>();
        }

        [[nodiscard]]
        std::shared_ptr<IEamRepository> createEamRepository() const {
            switch (_backend) {
                case BackendType::MONGODB:
                    return std::make_shared<MongoEamRepository>();
                case BackendType::MEMORY:
                    return std::make_shared<MemoryEamRepository>();
            }
            return std::make_shared<MemoryEamRepository>();
        }

        [[nodiscard]]
        std::shared_ptr<IEmoRepository> createEmoRepository() const {
            switch (_backend) {
                case BackendType::MONGODB:
                    return std::make_shared<MongoEmoRepository>();
                case BackendType::MEMORY:
                    return std::make_shared<MemoryEmoRepository>();
            }
            return std::make_shared<MemoryEmoRepository>();
        }

        [[nodiscard]]
        std::shared_ptr<IEtsRepository> createEtsRepository() const {
            switch (_backend) {
                case BackendType::MONGODB:
                    return std::make_shared<MongoEtsRepository>();
                case BackendType::MEMORY:
                    return std::make_shared<MemoryEtsRepository>();
            }
            return std::make_shared<MemoryEtsRepository>();
        }

        [[nodiscard]]
        std::shared_ptr<IEapRepository> createEapRepository() const {
            switch (_backend) {
                case BackendType::MONGODB:
                    return std::make_shared<MongoEapRepository>();
                case BackendType::MEMORY:
                    return std::make_shared<MemoryEapRepository>();
            }
            return std::make_shared<MemoryEapRepository>();
        }

        [[nodiscard]]
        std::shared_ptr<IEsmRepository> createEsmRepository() const {
            switch (_backend) {
                case BackendType::MONGODB:
                    return std::make_shared<MongoEsmRepository>();
                case BackendType::MEMORY:
                    return std::make_shared<MemoryEsmRepository>();
            }
            return std::make_shared<MemoryEsmRepository>();
        }

        [[nodiscard]]
        std::shared_ptr<IEkmRepository> createEkmRepository() const {
            switch (_backend) {
                case BackendType::MONGODB:
                    return std::make_shared<MongoEkmRepository>();
                case BackendType::MEMORY:
                    return std::make_shared<MemoryEkmRepository>();
            }
            return std::make_shared<MemoryEkmRepository>();
        }
    };

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
            const auto user = RepositoryFactory::instance().eamRepository()->findUserByAccessKeyId(accessKeyId);
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
            const auto repo = RepositoryFactory::instance().eamRepository();
            const auto user = repo->findUserByUserId(userId);
            if (!user.has_value()) return false;
            if (IsEamAdmin(*repo, userId)) return true;// global admin bypass
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
            const auto repo = RepositoryFactory::instance().eamRepository();
            const auto user = repo->findUserByUserId(userId);
            if (!user.has_value()) return false;
            // No list means no restriction: humans, and every user written before resource grants
            // existed, are unaffected. A principal that names resources is held to exactly them.
            if (user->resourceGrants.empty()) return true;
            return std::ranges::contains(user->resourceGrants, resourceErn);
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