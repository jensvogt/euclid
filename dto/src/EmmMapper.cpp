// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <euclid/dto/emm/EmmMapper.h>

#include <euclid/core/SystemUtils.h>

namespace Euclid::Dto {

    namespace {

        // Resolved once. boost::asio::ip::host_name() asks the OS on every call, and this runs on
        // every instance state change of every module; the name cannot change under a running
        // process in any way that matters here.
        const std::string &hostName() {
            static const std::string name = Core::SystemUtils::GetHostName();
            return name;
        }

    }// namespace

    Database::Entity::Module EmmMapper::toModuleEntity(const ModuleProcess &svc) {
        Database::Entity::Module m;
        m.name = svc.config.name;
        m.executable = svc.config.executable;
        m.socketPath = svc.config.socketPath;
        m.active = true;
        m.core = svc.config.core;
        m.args = svc.config.args;
        m.autoRestart = svc.config.autoRestart;
        m.maxRestarts = svc.config.maxRestarts;
        m.minInstances = svc.config.minInstances;
        m.maxInstances = svc.config.maxInstances;
        m.modified = std::chrono::system_clock::now();
        return m;
    }

    Database::Entity::ModuleInstance EmmMapper::toInstanceEntity(const ModuleProcess &svc) {
        Database::Entity::ModuleInstance instance;
        instance.instanceId = svc.instanceId;
        instance.pid = svc.pid;

        // The one place an instance record is written, so the one place the host has to be
        // stamped - every record this manager writes says which machine the pid belongs to.
        //
        // Read once: the name cannot change while the process runs, and this is called on every
        // state transition of every instance.
        instance.host = hostName();
        instance.state = svc.state;
        instance.socketPath = svc.instanceSocketPath;
        instance.httpPort = svc.httpPort;
        instance.restartCount = svc.restartCount;
        instance.modified = std::chrono::system_clock::now();
        return instance;
    }

} // namespace Euclid::Dto
