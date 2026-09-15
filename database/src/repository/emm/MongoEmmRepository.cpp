// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

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
            auto collection = Database::instance().collection(COLLECTION);

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

            auto collection = Database::instance().collection(COLLECTION);

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

            auto collection = Database::instance().collection(COLLECTION);

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

            auto _moduleCollection = Database::instance().collection(COLLECTION);
            if (auto mResult = _moduleCollection.find_one(bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp("name", name)))) {
                return Entity::Module::fromDocument(mResult->view());
            }

        } catch (const std::exception &e) {

            log_error << "Get module by name failed, name: " << name << " error: " << e.what();
        }
        return {};
    }

    std::vector<Entity::Module> MongoEmmRepository::findAll() const {

        try {

            auto collection = Database::instance().collection(COLLECTION);

            std::vector<Entity::Module> modules;
            for (auto cursor = collection.find({}); const auto &doc: cursor) {

                // Per document, not per listing. This answer drives every reconcile the manager
                // makes - what to start, what to stop, what to scale - and it used to be all or
                // nothing: one document that could not be read threw, the catch below answered
                // with an empty vector, and the manager could not tell that apart from an
                // installation with no modules at all.
                try {
                    modules.push_back(Entity::Module::fromDocument(doc));
                } catch (const std::exception &e) {
                    const auto name = doc["name"];
                    log_error << "Skipping unreadable module document, name: "
                              << (name && name.type() == bsoncxx::type::k_string ? std::string(name.get_string().value) : std::string("<unnamed>"))
                              << ", error: " << e.what();
                }
            }
            return modules;

        } catch (const std::exception &e) {

            log_error << "Get module names, error: " << e.what();
            return {};
        }
    }

    void MongoEmmRepository::clearInstanceReports(const std::string &moduleName, const std::string &instanceId) {

        try {
            auto collection = Database::instance().collection(COLLECTION);

            const auto filter = bsoncxx::builder::basic::make_document(
                    bsoncxx::builder::basic::kvp("name", moduleName),
                    bsoncxx::builder::basic::kvp("instances.instanceId", instanceId));

            // Back to what ModuleInstance's own defaults say, rather than removed: -1 is how
            // "never reported" is spelt for the two load figures, and a missing field would read
            // as zero - which for utilisation is a lie the autoscaler would act on.
            const auto update = bsoncxx::builder::basic::make_document(
                    bsoncxx::builder::basic::kvp("$set", bsoncxx::builder::basic::make_document(
                                                         bsoncxx::builder::basic::kvp("instances.$.backgroundTasks", static_cast<std::int64_t>(0)),
                                                         bsoncxx::builder::basic::kvp("instances.$.utilisation", -1.0),
                                                         bsoncxx::builder::basic::kvp("instances.$.backlog", static_cast<std::int64_t>(-1)))));

            std::ignore = collection.update_one(filter.view(), update.view());

        } catch (const std::exception &e) {
            // Not advisory in the way reporting is: leaving a stale count behind pins the instance
            // in the pool for good, so this is worth a warning rather than a debug line.
            log_warning << "Could not clear instance reports, module: " << moduleName
                        << ", instanceId: " << instanceId << ", error: " << e.what();
        }
    }

    void MongoEmmRepository::reportBackgroundTasks(const std::string &moduleName, const std::string &instanceId, const long tasks) {

        try {
            auto collection = Database::instance().collection(COLLECTION);

            // The positional operator, so this touches that one field of that one array element
            // and nothing else - the manager owns the rest of the record and writes it whole.
            const auto filter = bsoncxx::builder::basic::make_document(
                    bsoncxx::builder::basic::kvp("name", moduleName),
                    bsoncxx::builder::basic::kvp("instances.instanceId", instanceId));

            const auto update = bsoncxx::builder::basic::make_document(
                    bsoncxx::builder::basic::kvp("$set", bsoncxx::builder::basic::make_document(
                                                         bsoncxx::builder::basic::kvp("instances.$.backgroundTasks", static_cast<std::int64_t>(tasks)))));

            // No upsert: a module whose record the manager has not written yet was not started by
            // the manager, so there is no pool slot for this to belong to.
            std::ignore = collection.update_one(filter.view(), update.view());

        } catch (const std::exception &e) {
            // Reporting is advisory - the work carries on either way, and the worst case is the
            // autoscaler stopping an instance it would otherwise have spared.
            log_warning << "Could not report background tasks, module: " << moduleName
                        << ", instanceId: " << instanceId << ", error: " << e.what();
        }
    }

    void MongoEmmRepository::reportInstanceLoad(const std::string &moduleName, const std::string &instanceId,
                                                const double utilisation, const long backlog,
                                                const long activeHandlers) {

        try {
            auto collection = Database::instance().collection(COLLECTION);

            const auto filter = bsoncxx::builder::basic::make_document(
                    bsoncxx::builder::basic::kvp("name", moduleName),
                    bsoncxx::builder::basic::kvp("instances.instanceId", instanceId));

            // The timestamp goes with the figures and is set here rather than by the caller: an
            // instance with a skewed clock would otherwise report load that reads as stale, or as
            // fresh for ever.
            const auto update = bsoncxx::builder::basic::make_document(
                    bsoncxx::builder::basic::kvp("$set", bsoncxx::builder::basic::make_document(
                                                         bsoncxx::builder::basic::kvp("instances.$.utilisation", utilisation),
                                                         bsoncxx::builder::basic::kvp("instances.$.backlog", static_cast<std::int64_t>(backlog)),
                                                         // Work started and not finished, in the field scale-down already
                                                         // passes over. An application cannot reach reportBackgroundTasks()
                                                         // - it has no database - so its load report is how it says the same
                                                         // thing, and for a deployed pool this is the only writer.
                                                         bsoncxx::builder::basic::kvp("instances.$.backgroundTasks", static_cast<std::int64_t>(activeHandlers)),
                                                         bsoncxx::builder::basic::kvp("instances.$.loadReportedAt", bsoncxx::types::b_date{
                                                                                                                           std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                                                                                   std::chrono::system_clock::now().time_since_epoch())}))));

            std::ignore = collection.update_one(filter.view(), update.view());

        } catch (const std::exception &e) {
            // Advisory: a report that cannot be written costs the autoscaler one sample, and the
            // instance carries on doing the work either way.
            log_warning << "Could not report instance load, module: " << moduleName
                        << ", instanceId: " << instanceId << ", error: " << e.what();
        }
    }

    void MongoEmmRepository::upsertInstance(const Entity::Module &module, const Entity::ModuleInstance &instance) {

        try {

            auto collection = Database::instance().collection(COLLECTION);

            bsoncxx::builder::basic::document moduleFieldsDoc;
            moduleFieldsDoc.append(
                    bsoncxx::builder::basic::kvp("executable", module.executable),
                    bsoncxx::builder::basic::kvp("socketPath", module.socketPath),
                    bsoncxx::builder::basic::kvp("active", module.active),
                    bsoncxx::builder::basic::kvp("core", module.core),
                    bsoncxx::builder::basic::kvp("autoRestart", module.autoRestart),
                    bsoncxx::builder::basic::kvp("maxRestarts", module.maxRestarts),
                    bsoncxx::builder::basic::kvp("minInstances", module.minInstances),
                    bsoncxx::builder::basic::kvp("maxInstances", module.maxInstances),
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

                // Field by field rather than `kvp("instances.$", instance.toDocument())`.
                //
                // That replaced the whole array element, which deletes every field the replacement
                // does not carry - and two of them are not the manager's to carry. An instance
                // reports its own background-task count and its own load, because they are the two
                // things the manager cannot observe, and writing the subdocument whole erased both
                // on the next state change. The instance wrote them again on its next tick, so the
                // symptom was a figure that flickered to nothing rather than one that was never
                // there, which is worse to find.
                //
                // Spelling out what the manager owns says where the line is, and the two writers
                // stop overwriting each other.
                // Held in a named value: toDocument() returns by value, and iterating a view into
                // the temporary would be reading a document that has already been destroyed.
                const auto instanceFields = instance.toDocument();
                for (const auto &field: instanceFields.view()) {
                    setDoc.append(bsoncxx::builder::basic::kvp("instances.$." + std::string(field.key()), field.get_value()));
                }

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

    bool MongoEmmRepository::setDesiredInstances(const std::string &name, const int minInstances, const int maxInstances) {

        try {
            auto collection = Database::instance().collection(COLLECTION);

            // Only the two fields, and only the ones actually being set: -1 means "leave the
            // standing request alone", so raising a ceiling does not silently drop a floor.
            bsoncxx::builder::basic::document setDoc;
            if (minInstances >= 0) setDoc.append(bsoncxx::builder::basic::kvp("desiredMinInstances", minInstances));
            if (maxInstances >= 0) setDoc.append(bsoncxx::builder::basic::kvp("desiredMaxInstances", maxInstances));
            if (setDoc.view().empty()) return exists(name);

            const auto filter = bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp("name", name));
            const auto update = bsoncxx::builder::basic::make_document(
                    bsoncxx::builder::basic::kvp("$set", setDoc.extract()),
                    bsoncxx::builder::basic::kvp("$currentDate", bsoncxx::builder::basic::make_document(
                                                         bsoncxx::builder::basic::kvp("modified", true))));

            const auto result = collection.update_one(filter.view(), update.view());
            return result && result->matched_count() > 0;

        } catch (const std::exception &e) {
            log_error << "Set desired instances failed, name: " << name << ", error: " << e.what();
        }
        return false;
    }

    bool MongoEmmRepository::setDesiredThreads(const std::string &name, const int threads) {

        try {
            auto collection = Database::instance().collection(COLLECTION);

            const auto filter = bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp("name", name));
            const auto update = bsoncxx::builder::basic::make_document(
                    bsoncxx::builder::basic::kvp("$set", bsoncxx::builder::basic::make_document(
                                                         bsoncxx::builder::basic::kvp("desiredThreads", threads))),
                    bsoncxx::builder::basic::kvp("$currentDate", bsoncxx::builder::basic::make_document(
                                                         bsoncxx::builder::basic::kvp("modified", true))));

            const auto result = collection.update_one(filter.view(), update.view());
            return result && result->matched_count() > 0;

        } catch (const std::exception &e) {
            log_error << "Set desired threads failed, name: " << name << ", error: " << e.what();
        }
        return false;
    }

    bool MongoEmmRepository::setLogLevel(const std::string &name, const std::string &logLevel) {

        try {
            auto collection = Database::instance().collection(COLLECTION);

            const auto filter = bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp("name", name));
            const auto update = bsoncxx::builder::basic::make_document(
                    bsoncxx::builder::basic::kvp("$set", bsoncxx::builder::basic::make_document(
                                                         bsoncxx::builder::basic::kvp("logLevel", logLevel))),
                    bsoncxx::builder::basic::kvp("$currentDate", bsoncxx::builder::basic::make_document(
                                                         bsoncxx::builder::basic::kvp("modified", true))));

            const auto result = collection.update_one(filter.view(), update.view());
            return result && result->matched_count() > 0;

        } catch (const std::exception &e) {
            log_error << "Set log level failed, name: " << name << ", error: " << e.what();
        }
        return false;
    }

    bool MongoEmmRepository::setDesiredStopped(const std::string &name, const bool stopped) {

        try {
            auto collection = Database::instance().collection(COLLECTION);

            const auto filter = bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp("name", name));
            const auto update = bsoncxx::builder::basic::make_document(
                    bsoncxx::builder::basic::kvp("$set", bsoncxx::builder::basic::make_document(
                                                         bsoncxx::builder::basic::kvp("desiredStopped", stopped))),
                    bsoncxx::builder::basic::kvp("$currentDate", bsoncxx::builder::basic::make_document(
                                                         bsoncxx::builder::basic::kvp("modified", true))));

            const auto result = collection.update_one(filter.view(), update.view());
            return result && result->matched_count() > 0;

        } catch (const std::exception &e) {
            log_error << "Set desired stopped failed, name: " << name << ", error: " << e.what();
        }
        return false;
    }

    bool MongoEmmRepository::requestRestart(const std::string &name) {

        try {
            auto collection = Database::instance().collection(COLLECTION);

            const auto filter = bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp("name", name));
            // $currentDate for both, so the moment recorded is the server's - the manager compares
            // it against what it has already acted on, and two clocks disagreeing by a few seconds
            // is not something that comparison should have to survive.
            const auto update = bsoncxx::builder::basic::make_document(
                    bsoncxx::builder::basic::kvp("$currentDate", bsoncxx::builder::basic::make_document(
                                                         bsoncxx::builder::basic::kvp("restartRequestedAt", true),
                                                         bsoncxx::builder::basic::kvp("modified", true))));

            const auto result = collection.update_one(filter.view(), update.view());
            return result && result->matched_count() > 0;

        } catch (const std::exception &e) {
            log_error << "Request restart failed, name: " << name << ", error: " << e.what();
        }
        return false;
    }

    void MongoEmmRepository::removeInstance(const std::string &moduleName, const std::string &instanceId) {

        try {
            auto collection = Database::instance().collection(COLLECTION);

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

            auto collection = Database::instance().collection(COLLECTION);

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
            auto collection = Database::instance().collection(COLLECTION);

            const auto result = collection.delete_many(bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp("name", name)));
            log_debug << "Module deleted, count: " << result->deleted_count();

        } catch (const std::exception &e) {

            log_error << "Delete module failed, error: " << e.what();
        }

    }

    void MongoEmmRepository::clear() {

        try {
            auto collection = Database::instance().collection(COLLECTION);

            const auto result = collection.delete_many({});
            log_debug << "All module deleted, count: " << result->deleted_count();

        } catch (const std::exception &e) {
            log_error << "Delete all module failed, error: " << e.what();
        }
    }

}// namespace Euclid::Database