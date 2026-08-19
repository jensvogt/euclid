//
// Created by vogje01 on 5/24/26.
//

#pragma once

// C++ includes
#include <memory>

// Euclid includes
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/monitoring/MetricsPusher.h>
#include <euclid/database/repository/access/IAccessRepository.h>
#include <euclid/database/repository/access/MemoryAccessRepository.h>
#include <euclid/database/repository/access/MongoAccessRepository.h>
#include <euclid/database/repository/sqs/ISQSRepository.h>
#include <euclid/database/repository/sqs/MemorySQSRepository.h>
#include <euclid/database/repository/sqs/MongoSQSRepository.h>
#include <euclid/database/repository/module/IModuleRepository.h>
#include <euclid/database/repository/module/MongoModuleRepository.h>
#include <euclid/database/repository/module/MemoryModuleRepository.h>
#include <euclid/database/repository/monitoring/IMonitoringRepository.h>
#include <euclid/database/repository/monitoring/MongoMonitoringRepository.h>
#include <euclid/database/repository/monitoring/MemoryMonitoringRepository.h>

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
        }

        [[nodiscard]]
        std::shared_ptr<IModuleRepository> moduleRepository() const {
            static auto repo = createModuleRepository();
            return repo;
        }

        [[nodiscard]]
        std::shared_ptr<ISQSRepository> sqsRepository() const {
            static auto repo = createSQSRepository();
            return repo;
        }

        [[nodiscard]]
        std::shared_ptr<IAccessRepository> accessRepository() const {
            static auto repo = createAccessRepository();
            return repo;
        }

        [[nodiscard]]
        std::shared_ptr<IMonitoringRepository> monitoringRepository() const {
            static auto repo = createMonitoringRepository();
            return repo;
        }

    private:
        BackendType _backend = BackendType::MONGODB;

        [[nodiscard]]
        std::shared_ptr<IModuleRepository> createModuleRepository() const {
            switch (_backend) {
                case BackendType::MONGODB:
                    return std::make_shared<MongoModuleRepository>();
                case BackendType::MEMORY:
                    return std::make_shared<MemoryModuleRepository>();
            }
            return std::make_shared<MemoryModuleRepository>();
        }

        [[nodiscard]]
        std::shared_ptr<ISQSRepository> createSQSRepository() const {
            switch (_backend) {
                case BackendType::MONGODB:
                    return std::make_shared<MongoSQSRepository>();
                case BackendType::MEMORY:
                    return std::make_shared<MemorySQSRepository>();
            }
            return std::make_shared<MemorySQSRepository>();
        }

        [[nodiscard]]
        std::shared_ptr<IAccessRepository> createAccessRepository() const {
            switch (_backend) {
                case BackendType::MONGODB:
                    return std::make_shared<MongoAccessRepository>();
                case BackendType::MEMORY:
                    return std::make_shared<MemoryAccessRepository>();
            }
            return std::make_shared<MemoryAccessRepository>();
        }

        [[nodiscard]]
        std::shared_ptr<IMonitoringRepository> createMonitoringRepository() const {
            switch (_backend) {
                case BackendType::MONGODB:
                    return std::make_shared<MongoMonitoringRepository>();
                case BackendType::MEMORY:
                    return std::make_shared<MemoryMonitoringRepository>();
            }
            return std::make_shared<MemoryMonitoringRepository>();
        }
    };

    /**
     * @brief Registers the SigV4 access-key lookup Core::HttpActionServer::Authenticate() needs,
     * backed by RepositoryFactory::accessRepository().
     *
     * core can't depend on database (database depends on core), so this is the glue that closes
     * the loop - call once per process, after RepositoryFactory::initialize(), in every
     * executable whose HttpActionServer-derived server needs to verify SigV4-signed requests
     * (the gateway and every AWS-service module).
     */
    inline void WireAccessKeyLookup() {
        Core::HttpActionServer::SetAccessKeyLookup([](const std::string &accessKeyId) -> std::optional<Core::HttpActionServer::AccessKeyRecord> {
            const auto user = RepositoryFactory::instance().accessRepository()->findUserByAccessKeyId(accessKeyId);
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
            for (const auto &instance: RepositoryFactory::instance().moduleRepository()->findAll()) {
                if (instance.name != moduleName || instance.state != Entity::ModuleState::RUNNING) continue;
                sockets.push_back(instance.socketPath);
            }
            return sockets;
        });
    }

} // namespace Euclid::Database