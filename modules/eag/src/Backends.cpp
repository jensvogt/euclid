//
// Created by vogje01 on 9/5/26.
//

// Euclid includes
#include <Backends.h>
#include <euclid/core/LogStream.h>
#include <euclid/database/RepositoryFactory.h>

namespace Euclid::EAG {

    void Backends::refresh(const std::vector<std::string> &applicationIds) {

        std::map<std::string, std::vector<int> > ports;
        try {
            const auto repository = Database::RepositoryFactory::instance().emmRepository();
            for (const auto &applicationId: applicationIds) {
                const auto module = repository->findByName(applicationId);
                if (!module.has_value()) continue;

                for (const auto &instance: module->instances) {
                    if (instance.state != Database::Entity::ModuleState::RUNNING) continue;

                    // An instance with no port is one the manager could not give one to - either
                    // no range is configured, or the range is exhausted. Sending a request to port
                    // zero would fail in a way that reads as the application being broken, so it
                    // is left out of the rotation and says so once, here.
                    if (instance.httpPort <= 0) {
                        log_debug << "Instance has no HTTP port and cannot be routed to, application: " << applicationId
                                  << ", instance: " << instance.instanceId;
                        continue;
                    }
                    ports[applicationId].push_back(instance.httpPort);
                }
            }
        } catch (const std::exception &e) {
            log_error << "Could not refresh the backend list, keeping the previous one, error: " << e.what();
            return;
        }

        std::lock_guard lock(_mutex);
        _ports = std::move(ports);
    }

    std::optional<int> Backends::next(const std::string &applicationId) {

        std::lock_guard lock(_mutex);
        const auto it = _ports.find(applicationId);
        if (it == _ports.end() || it->second.empty()) return std::nullopt;

        auto &cursor = _cursors[applicationId];
        const auto port = it->second[cursor % it->second.size()];
        cursor++;
        return port;
    }

    std::size_t Backends::count(const std::string &applicationId) const {
        std::lock_guard lock(_mutex);
        const auto it = _ports.find(applicationId);
        return it != _ports.end() ? it->second.size() : 0;
    }

}// namespace Euclid::EAG
