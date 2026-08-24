//
// Created by vogje01 on 03/09/2023.
//

#include <euclid/database/entity/emm/Module.h>

namespace Euclid::Database::Entity {

    bsoncxx::document::value ModuleInstance::toDocument() const {

        return bsoncxx::builder::basic::make_document(
                bsoncxx::builder::basic::kvp("instanceId", instanceId),
                bsoncxx::builder::basic::kvp("pid", pid),
                bsoncxx::builder::basic::kvp("state", ModuleStateToString(state)),
                bsoncxx::builder::basic::kvp("socketPath", socketPath),
                bsoncxx::builder::basic::kvp("restartCount", restartCount),
                bsoncxx::builder::basic::kvp("created", bsoncxx::types::b_date{std::chrono::duration_cast<std::chrono::milliseconds>(created.time_since_epoch())}),
                bsoncxx::builder::basic::kvp("modified", bsoncxx::types::b_date{std::chrono::duration_cast<std::chrono::milliseconds>(modified.time_since_epoch())}));
    }

    ModuleInstance ModuleInstance::fromDocument(const bsoncxx::document::view &doc) {
        ModuleInstance instance;
        for (const auto &field: doc) {
            if (const auto key = field.key(); key == "instanceId") instance.instanceId = std::string(field.get_string().value);
            else if (key == "pid") instance.pid = field.get_int32().value;
            else if (key == "state") instance.state = ModuleStateFromString(std::string(field.get_string().value));
            else if (key == "socketPath") instance.socketPath = std::string(field.get_string().value);
            else if (key == "restartCount") instance.restartCount = field.get_int32().value;
            else if (key == "created") instance.created = std::chrono::system_clock::time_point{field.get_date().value};
            else if (key == "modified") instance.modified = std::chrono::system_clock::time_point{field.get_date().value};
        }
        return instance;
    }

    bsoncxx::document::value Module::toDocument() const {

        return bsoncxx::builder::basic::make_document(
                bsoncxx::builder::basic::kvp("name", name),
                bsoncxx::builder::basic::kvp("executable", executable),
                bsoncxx::builder::basic::kvp("socketPath", socketPath),
                bsoncxx::builder::basic::kvp("active", active),
                bsoncxx::builder::basic::kvp("autoRestart", autoRestart),
                bsoncxx::builder::basic::kvp("maxRestarts", maxRestarts),
                bsoncxx::builder::basic::kvp("args", [this](bsoncxx::builder::basic::sub_array sa) {
                    for (const auto &arg: args) sa.append(arg);
                }),
                bsoncxx::builder::basic::kvp("instances", [this](bsoncxx::builder::basic::sub_array sa) {
                    for (const auto &instance: instances) sa.append(instance.toDocument());
                }));
    }

    Module Module::fromDocument(const std::optional<bsoncxx::document::view> &doc) {
        if (!doc) return {};

        Module module;
        for (const auto &field: *doc) {
            if (const auto key = field.key(); key == "name") module.name = std::string(field.get_string().value);
            else if (key == "executable") module.executable = std::string(field.get_string().value);
            else if (key == "socketPath") module.socketPath = std::string(field.get_string().value);
            else if (key == "active") module.active = field.get_bool().value;
            else if (key == "autoRestart") module.autoRestart = field.get_bool().value;
            else if (key == "maxRestarts") module.maxRestarts = field.get_int32().value;
            else if (key == "args") {
                for (const auto &elem: field.get_array().value) module.args.push_back(std::string(elem.get_string().value));
            } else if (key == "instances") {
                for (const auto &elem: field.get_array().value) module.instances.push_back(ModuleInstance::fromDocument(elem.get_document().value));
            } else if (key == "created") module.created = std::chrono::system_clock::time_point{field.get_date().value};
            else if (key == "modified") module.modified = std::chrono::system_clock::time_point{field.get_date().value};
            else if (key == "_id") module.oid = field.get_oid().value.to_string();
        }
        return module;
    }

}// namespace Euclid::Database::Entity::Module
