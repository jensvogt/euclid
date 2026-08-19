//
// Created by vogje01 on 03/09/2023.
//

#include <euclid/database/entity/module/Module.h>

namespace Euclid::Database::Entity {

    bsoncxx::document::value Module::toDocument() const {

        return bsoncxx::builder::basic::make_document(
                bsoncxx::builder::basic::kvp("name", name),
                bsoncxx::builder::basic::kvp("executable", executable),
                bsoncxx::builder::basic::kvp("socketPath", socketPath),
                bsoncxx::builder::basic::kvp("active", active),
                bsoncxx::builder::basic::kvp("state", ModuleStateToString(state)),
                bsoncxx::builder::basic::kvp("pid", static_cast<int32_t>(pid)),
                bsoncxx::builder::basic::kvp("restartCount", restartCount),
                bsoncxx::builder::basic::kvp("autoRestart", autoRestart),
                bsoncxx::builder::basic::kvp("maxRestarts", maxRestarts),
                bsoncxx::builder::basic::kvp("args", [this](bsoncxx::builder::basic::sub_array sa) {
                    for (const auto &arg: args) sa.append(arg);
                }));
    }

    Module Module::fromDocument(const std::optional<bsoncxx::document::view> &doc) {
        if (!doc) return {};

        Module module;
        for (const auto &field: *doc) {
            if (const auto key = field.key(); key == "name") module.name = std::string(field.get_string().value);
            else if (key == "executable") module.executable = std::string(field.get_string().value);
            else if (key == "socketPath") module.socketPath = std::string(field.get_string().value);
            else if (key == "state") module.state = ModuleStateFromString(std::string(field.get_string().value));
            else if (key == "pid") module.pid = field.get_int32().value;
            else if (key == "restartCount") module.restartCount = field.get_int32().value;
            else if (key == "autoRestart") module.autoRestart = field.get_bool().value;
            else if (key == "maxRestarts") module.maxRestarts = field.get_int32().value;
            else if (key == "args") {
                for (const auto &elem: field.get_array().value) module.args.push_back(std::string(elem.get_string().value));
            } else if (key == "created") module.created = std::chrono::system_clock::time_point{field.get_date().value};
            else if (key == "modified") module.modified = std::chrono::system_clock::time_point{field.get_date().value};
            else if (key == "_id") module.oid = field.get_oid().value.to_string();
        }
        return module;
    }

}// namespace Euclid::Database::Entity::Module