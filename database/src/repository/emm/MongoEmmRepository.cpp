//
// Created by vogje01 on 29/05/2023.
//

#include <euclid/database/repository/emm/MongoEmmRepository.h>

namespace Euclid::Database {

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
            // auto collection = (*entry)[_databaseName][_moduleCollectionName];
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
            auto collection = (*entry)[_databaseName][_moduleCollectionName];

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

    // Entity::Module MongoModuleRepository::upsert(const Entity::Module &module) const {
    void MongoEmmRepository::upsert(const Entity::Module &module) {

        try {

            mongocxx::options::update upsertOpt;
            upsertOpt.upsert(true);

            const auto entry = Database::instance().client();
            auto collection = (*entry)[_databaseName][_moduleCollectionName];

            // Filters on name+pid (not just name) so that each instance of an autoscaled module
            // keeps its own status document instead of clobbering one another - monitoring's
            // instance discovery (MongoModuleRepository::findAll()) depends on this.
            const auto filter = bsoncxx::builder::basic::make_document(
                    bsoncxx::builder::basic::kvp("name", module.name),
                    bsoncxx::builder::basic::kvp("pid", static_cast<int32_t>(module.pid)));
            const auto update = bsoncxx::builder::basic::make_document(
                    bsoncxx::builder::basic::kvp("$set", module.toDocument()),
                    bsoncxx::builder::basic::kvp("$setOnInsert", bsoncxx::builder::basic::make_document(
                                                         bsoncxx::builder::basic::kvp("created", bsoncxx::types::b_date{
                                                                                              std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                                                      module.created.time_since_epoch())})
                                                         )),
                    bsoncxx::builder::basic::kvp("$currentDate", bsoncxx::builder::basic::make_document(
                                                         bsoncxx::builder::basic::kvp("modified", true)
                                                         )));

            mongocxx::options::update opts;
            opts.upsert(true);
            collection.update_one(filter.view(), update.view(), opts);

        } catch (const std::exception &e) {

            log_error << "Update module failed, error: " << e.what();
            //                throw Core::DatabaseException("Update module failed, error: " + std::string(e.what()));
        }
        // return {};
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