#include <euclid/dto/emm/EmmMapper.h>

namespace Euclid::Dto {

    Database::Entity::Module EmmMapper::toEntity(const ModuleProcess &svc) {
        Database::Entity::Module m;
        m.name = svc.config.name;
        m.executable = svc.config.executable;
        m.socketPath = svc.instanceSocketPath;
        m.active = svc.config.autoRestart;
        m.state = svc.state;
        m.pid = svc.pid;
        m.args = svc.config.args;
        m.restartCount = svc.restartCount;
        m.autoRestart = svc.config.autoRestart;
        m.maxRestarts = svc.config.maxRestarts;
        m.modified = std::chrono::system_clock::now();
        return m;
    }

    ModuleProcess EmmMapper::toDto(const Database::Entity::Module &m) {
        ModuleProcess svc;
        svc.config.name = m.name;
        svc.config.executable = m.executable;
        svc.config.socketPath = m.socketPath;
        svc.config.args = {m.socketPath};
        svc.config.autoRestart = m.autoRestart;
        svc.config.maxRestarts = m.maxRestarts;
        svc.config.args = m.args;
        svc.pid = m.pid;
        svc.restartCount = m.restartCount;
        svc.state = m.state;
        return svc;
    }

} // namespace Euclid::Dto