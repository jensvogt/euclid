// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <euclid/dto/emm/EmmMapper.h>

namespace Euclid::Dto {

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
        instance.state = svc.state;
        instance.socketPath = svc.instanceSocketPath;
        instance.httpPort = svc.httpPort;
        instance.restartCount = svc.restartCount;
        instance.modified = std::chrono::system_clock::now();
        return instance;
    }

} // namespace Euclid::Dto
