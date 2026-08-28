//
// Created by vogje01 on 29/05/2023.
//

// Mongo Db includes
#include <bsoncxx/builder/concatenate.hpp>

// Euclid includes
#include <euclid/database/repository/emm/MongoEmmRepository.h>

namespace Euclid::Database {

    MongoEmmRepository::MongoEmmRepository() {
        ensureIndexes();
    }

    void MongoEmmRepository::ensureIndexes() {

        try {
            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];

            // One document per module name by design (see Module.h) - nearly every read/write
            // filters on it. Not worth also indexing instances.instanceId: once name narrows to
            // (at most) a single document, indexing the array field within it buys nothing.
            mongocxx::options::index nameOpts;
            nameOpts.unique(true);
            collection.create_index(bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp("name", 1)), nameOpts);

        } catch (const std::exception &e) {
            log_error << "Ensure module indexes failed, error: " << e.what();
        }
    }

    bool MongoEmmRepository::exists(const std::string &name) const {

        try {

            bsoncxx::builder::basic::document query{};
            if (!name.empty()) {
                query.append(bsoncxx::builder::basic::kvp("name", name));
            }

            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];

            const auto result = collection.find_one(query.extract());
            log_trace << "Module exists, name: " << name << ", exists: " << std::boolalpha << result.has_value();
            return result.has_value();

        } catch (const std::exception &e) {
            log_error << "Module exists failed, name: " << ", error: " << e.what();
        }
        return false;
    }

    std::optional<Entity::Module> MongoEmmRepository::findById(const std::string &oid) const {

        try {

            bsoncxx::builder::basic::document document;
            document.append(bsoncxx::builder::basic::kvp("_id", oid));

            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];

            if (auto mResult = collection.find_one(document.view())) {
                return Entity::Module::fromDocument(mResult->view());
            }

        } catch (const std::exception &e) {
            log_error << "Get module by ID failed, error: " << e.what();
        }
        return {};
    }

    std::optional<Entity::Module> MongoEmmRepository::findByName(const std::string &name) const {

        try {

            // const auto entry = Database::instance().client();
            // auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];
            // if (auto mResult = _moduleCollection.find_one(make_document(kvp("name", name)))) {
            //     Entity::Module modules;
            //     modules.FromDocument(mResult->view());
            //     return modules;
            // }

        } catch (const std::exception &e) {

            log_error << "Get module by name failed, name: " << name << " error: " << e.what();
        }
        return {};
    }

    std::vector<Entity::Module> MongoEmmRepository::findAll() const {

        try {

            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];

            std::vector<Entity::Module> modules;
            for (auto cursor = collection.find({}); const auto &doc: cursor) {
                modules.push_back(Entity::Module::fromDocument(doc));
            }
            return modules;

        } catch (const std::exception &e) {

            log_error << "Get module names, error: " << e.what();
            return {};
        }
    }

    void MongoEmmRepository::upsertInstance(const Entity::Module &module, const Entity::ModuleInstance &instance) {

        try {

            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];

            bsoncxx::builder::basic::document moduleFieldsDoc;
            moduleFieldsDoc.append(
                    bsoncxx::builder::basic::kvp("executable", module.executable),
                    bsoncxx::builder::basic::kvp("socketPath", module.socketPath),
                    bsoncxx::builder::basic::kvp("active", module.active),
                    bsoncxx::builder::basic::kvp("autoRestart", module.autoRestart),
                    bsoncxx::builder::basic::kvp("maxRestarts", module.maxRestarts),
                    bsoncxx::builder::basic::kvp("args", [&module](bsoncxx::builder::basic::sub_array sa) {
                        for (const auto &arg: module.args) sa.append(arg);
                    }));

            // Stamps the module's "boot time" - read fresh from the currently-persisted document
            // rather than tracked in ServiceController's in-memory state, so it's correct across
            // manager restarts and uniform across every path that can bring an instance up
            // (initial start, crash-restart, scale-up from zero). Only set when this transition is
            // the pool's first RUNNING instance (no other instance in the stored document is
            // already RUNNING); a module with several instances starting up together only stamps
            // once, on whichever of them reaches RUNNING first.
            if (instance.state == Entity::ModuleState::RUNNING) {
                bool anyOtherRunning = false;
                if (const auto existing = collection.find_one(bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp("name", module.name)))) {
                    if (const auto instancesField = existing->view()["instances"]; instancesField && instancesField.type() == bsoncxx::type::k_array) {
                        for (const auto &elem: instancesField.get_array().value) {
                            const auto other = Entity::ModuleInstance::fromDocument(elem.get_document().value);
                            if (other.instanceId != instance.instanceId && other.state == Entity::ModuleState::RUNNING) {
                                anyOtherRunning = true;
                                break;
                            }
                        }
                    }
                }
                if (!anyOtherRunning) {
                    moduleFieldsDoc.append(bsoncxx::builder::basic::kvp("lastStartTime", bsoncxx::types::b_date{
                                                                                 std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())}));
                }
            }
            const auto moduleFields = moduleFieldsDoc.extract();

            // Step 1: the instance is already in the array (a restart of an existing pool slot,
            // matched by its stable instanceId rather than pid, which changes every restart) -
            // update it and the module-level fields in place via the positional $ operator.
            {
                const auto filter = bsoncxx::builder::basic::make_document(
                        bsoncxx::builder::basic::kvp("name", module.name),
                        bsoncxx::builder::basic::kvp("instances.instanceId", instance.instanceId));

                bsoncxx::builder::basic::document setDoc;
                setDoc.append(bsoncxx::builder::concatenate(moduleFields.view()));
                setDoc.append(bsoncxx::builder::basic::kvp("instances.$", instance.toDocument()));

                const auto update = bsoncxx::builder::basic::make_document(
                        bsoncxx::builder::basic::kvp("$set", setDoc.extract()),
                        bsoncxx::builder::basic::kvp("$currentDate", bsoncxx::builder::basic::make_document(
                                                             bsoncxx::builder::basic::kvp("modified", true))));

                const auto result = collection.update_one(filter.view(), update.view());
                if (result && result->matched_count() > 0) return;
            }

            // Step 2: new instance (either the module document doesn't exist yet, or it does but
            // this instanceId isn't in its array yet) - upsert the module document and push the
            // instance. Splitting this from step 1 is necessary because MongoDB's positional $
            // operator only ever updates an array element that already matched the query filter;
            // it can't be used to insert a not-yet-present element.
            const auto filter = bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp("name", module.name));

            bsoncxx::builder::basic::document setDoc;
            setDoc.append(bsoncxx::builder::basic::kvp("name", module.name));
            setDoc.append(bsoncxx::builder::concatenate(moduleFields.view()));

            const auto update = bsoncxx::builder::basic::make_document(
                    bsoncxx::builder::basic::kvp("$set", setDoc.extract()),
                    bsoncxx::builder::basic::kvp("$setOnInsert", bsoncxx::builder::basic::make_document(
                                                         bsoncxx::builder::basic::kvp("created", bsoncxx::types::b_date{
                                                                                              std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                                                      std::chrono::system_clock::now().time_since_epoch())}))),
                    bsoncxx::builder::basic::kvp("$push", bsoncxx::builder::basic::make_document(
                                                         bsoncxx::builder::basic::kvp("instances", instance.toDocument()))),
                    bsoncxx::builder::basic::kvp("$currentDate", bsoncxx::builder::basic::make_document(
                                                         bsoncxx::builder::basic::kvp("modified", true))));

            mongocxx::options::update opts;
            opts.upsert(true);
            collection.update_one(filter.view(), update.view(), opts);

        } catch (const std::exception &e) {
            log_error << "Upsert module instance failed, error: " << e.what();
        }
    }

    void MongoEmmRepository::removeInstance(const std::string &moduleName, const std::string &instanceId) {

        try {
            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];

            const auto filter = bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp("name", moduleName));
            const auto update = bsoncxx::builder::basic::make_document(
                    bsoncxx::builder::basic::kvp("$pull", bsoncxx::builder::basic::make_document(
                                                         bsoncxx::builder::basic::kvp("instances", bsoncxx::builder::basic::make_document(
                                                                                              bsoncxx::builder::basic::kvp("instanceId", instanceId))))),
                    bsoncxx::builder::basic::kvp("$currentDate", bsoncxx::builder::basic::make_document(
                                                         bsoncxx::builder::basic::kvp("modified", true))));

            collection.update_one(filter.view(), update.view());

        } catch (const std::exception &e) {
            log_error << "Remove module instance failed, error: " << e.what();
        }
    }

    long MongoEmmRepository::count() const {

        try {

            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];

            const int64_t count = collection.count_documents({});
            log_trace << "Service state: " << std::boolalpha << count;
            return static_cast<int>(count);

        } catch (const std::exception &e) {

            log_error << "Service exists failed, error: " << e.what();
        }
        return -1;
    }

    void MongoEmmRepository::remove(const std::string &name) {

        try {
            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];

            const auto result = collection.delete_many(bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp("name", name)));
            log_debug << "Module deleted, count: " << result->deleted_count();

        } catch (const std::exception &e) {

            log_error << "Delete module failed, error: " << e.what();
        }

    }

    void MongoEmmRepository::clear() {

        try {
            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];

            const auto result = collection.delete_many({});
            log_debug << "All module deleted, count: " << result->deleted_count();

        } catch (const std::exception &e) {
            log_error << "Delete all module failed, error: " << e.what();
        }
    }

}// namespace Euclid::Database