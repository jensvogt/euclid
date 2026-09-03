//
// Created by vogje01 on 03/09/2023.
//

#include <euclid/database/entity/emm/Module.h>

namespace Euclid::Database::Entity {

    namespace {
        // Instance counts, restart counters and pids are written as BSON int32 here, so today every
        // stored document reads back cleanly - but get_int32() refuses an int64 rather than
        // narrowing it, so the moment one of these fields is widened, or a document arrives from a
        // migration, a hand edit or a tool that writes int64, the mismatch throws out of
        // fromDocument() and the repository's catch-all turns it into an empty module list. Reading
        // either width costs nothing and removes that trap in advance; the same helper guards the
        // ETS, EAP, ESM and EQS entities, where it was added after exactly that failure.
        long getBsonInt(const bsoncxx::document::element &field) {
            if (field.type() == bsoncxx::type::k_int64) return field.get_int64().value;
            if (field.type() == bsoncxx::type::k_int32) return field.get_int32().value;
            return 0;
        }
    }// namespace

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
            else if (key == "pid") instance.pid = static_cast<int>(getBsonInt(field));
            else if (key == "state") instance.state = ModuleStateFromString(std::string(field.get_string().value));
            else if (key == "socketPath") instance.socketPath = std::string(field.get_string().value);
            else if (key == "restartCount") instance.restartCount = static_cast<int>(getBsonInt(field));
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
                bsoncxx::builder::basic::kvp("minInstances", minInstances),
                bsoncxx::builder::basic::kvp("maxInstances", maxInstances),
                bsoncxx::builder::basic::kvp("desiredMinInstances", desiredMinInstances),
                bsoncxx::builder::basic::kvp("desiredMaxInstances", desiredMaxInstances),
                bsoncxx::builder::basic::kvp("desiredThreads", desiredThreads),
                bsoncxx::builder::basic::kvp("core", core),
                bsoncxx::builder::basic::kvp("desiredStopped", desiredStopped),
                bsoncxx::builder::basic::kvp("restartRequestedAt", bsoncxx::types::b_date{std::chrono::duration_cast<std::chrono::milliseconds>(restartRequestedAt.time_since_epoch())}),
                bsoncxx::builder::basic::kvp("args", [this](bsoncxx::builder::basic::sub_array sa) {
                    for (const auto &arg: args) sa.append(arg);
                }),
                bsoncxx::builder::basic::kvp("lastStartTime", bsoncxx::types::b_date{std::chrono::duration_cast<std::chrono::milliseconds>(lastStartTime.time_since_epoch())}),
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
            else if (key == "maxRestarts") module.maxRestarts = static_cast<int>(getBsonInt(field));
            else if (key == "minInstances") module.minInstances = static_cast<int>(getBsonInt(field));
            else if (key == "maxInstances") module.maxInstances = static_cast<int>(getBsonInt(field));
            // Absent from a document written before set-instances existed, which reads as "nothing
            // was asked for" - the field's own default.
            else if (key == "desiredMinInstances") module.desiredMinInstances = static_cast<int>(getBsonInt(field));
            else if (key == "desiredMaxInstances") module.desiredMaxInstances = static_cast<int>(getBsonInt(field));
            else if (key == "desiredThreads") module.desiredThreads = static_cast<int>(getBsonInt(field));
            else if (key == "core") module.core = field.get_bool().value;
            else if (key == "desiredStopped") module.desiredStopped = field.get_bool().value;
            else if (key == "restartRequestedAt") module.restartRequestedAt = std::chrono::system_clock::time_point{field.get_date().value};
            else if (key == "args") {
                for (const auto &elem: field.get_array().value) module.args.push_back(std::string(elem.get_string().value));
            } else if (key == "instances") {
                for (const auto &elem: field.get_array().value) module.instances.push_back(ModuleInstance::fromDocument(elem.get_document().value));
            } else if (key == "created") module.created = std::chrono::system_clock::time_point{field.get_date().value};
            else if (key == "modified") module.modified = std::chrono::system_clock::time_point{field.get_date().value};
            else if (key == "lastStartTime") module.lastStartTime = std::chrono::system_clock::time_point{field.get_date().value};
            else if (key == "_id") module.oid = field.get_oid().value.to_string();
        }
        return module;
    }

}// namespace Euclid::Database::Entity::Module
