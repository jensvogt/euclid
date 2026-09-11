// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/5/26.
//

// Euclid includes
#include <Backends.h>
#include <euclid/core/LogStream.h>
#include <euclid/database/entity/eap/Application.h>
#include <euclid/database/RepositoryFactory.h>

namespace Euclid::EAG {

    void Backends::refresh(const std::vector<ApplicationRef> &applications) {

        std::map<std::string, std::vector<int> > ports;
        try {
            const auto modules = Database::RepositoryFactory::instance().emmRepository();
            const auto applicationRepository = Database::RepositoryFactory::instance().eapRepository();

            for (const auto &application: applications) {

                // Two lookups rather than one, because a route names an application the way a
                // person does and the module registry knows it by the name it runs under. Done
                // here, on the timer, so that serving a request needs neither of them.
                const auto definition = applicationRepository->findApplicationByApplicationId(
                        application.accountId, application.nameSpace, application.applicationId);
                if (!definition.has_value()) continue;

                const auto module = modules->findByName(Database::Entity::EAP::RuntimeName(*definition));
                if (!module.has_value()) continue;

                for (const auto &instance: module->instances) {
                    if (instance.state != Database::Entity::ModuleState::RUNNING) continue;

                    // An instance with no port is one the manager could not give one to - either
                    // no range is configured, or the range is exhausted. Sending a request to port
                    // zero would fail in a way that reads as the application being broken, so it
                    // is left out of the rotation and says so once, here.
                    if (instance.httpPort <= 0) {
                        log_debug << "Instance has no HTTP port and cannot be routed to, application: " << application.applicationId
                                  << ", instance: " << instance.instanceId;
                        continue;
                    }
                    ports[application.key()].push_back(instance.httpPort);
                }
            }
        } catch (const std::exception &e) {
            log_error << "Could not refresh the backend list, keeping the previous one, error: " << e.what();
            return;
        }

        std::lock_guard lock(_mutex);
        _ports = std::move(ports);
    }

    std::optional<int> Backends::next(const ApplicationRef &application) {

        const auto key = application.key();

        std::lock_guard lock(_mutex);
        const auto it = _ports.find(key);
        if (it == _ports.end() || it->second.empty()) return std::nullopt;

        auto &cursor = _cursors[key];
        const auto port = it->second[cursor % it->second.size()];
        cursor++;
        return port;
    }

    std::size_t Backends::count(const ApplicationRef &application) const {
        std::lock_guard lock(_mutex);
        const auto it = _ports.find(application.key());
        return it != _ports.end() ? it->second.size() : 0;
    }

}// namespace Euclid::EAG
