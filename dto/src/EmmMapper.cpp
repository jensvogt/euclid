#include <euclid/dto/emm/EmmMapper.h>

namespace Euclid::Dto {

    Database::Entity::Module EmmMapper::toModuleEntity(const ModuleProcess &svc) {
        Database::Entity::Module m;
        m.name = svc.config.name;
        m.executable = svc.config.executable;
        m.socketPath = svc.config.socketPath;
        m.active = true;
        m.args = svc.config.args;
        m.autoRestart = svc.config.autoRestart;
        m.maxRestarts = svc.config.maxRestarts;
        m.modified = std::chrono::system_clock::now();
        return m;
    }

    Database::Entity::ModuleInstance EmmMapper::toInstanceEntity(const ModuleProcess &svc) {
        Database::Entity::ModuleInstance instance;
        instance.instanceId = svc.instanceId;
        instance.pid = svc.pid;
        instance.state = svc.state;
        instance.socketPath = svc.instanceSocketPath;
        instance.restartCount = svc.restartCount;
        instance.modified = std::chrono::system_clock::now();
        return instance;
    }

} // namespace Euclid::Dto
