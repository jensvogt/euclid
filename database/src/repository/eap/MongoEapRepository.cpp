//
// Created by vogje01 on 9/1/26.
//

#include <euclid/database/repository/eap/MongoEapRepository.h>

namespace Euclid::Database {

    MongoEapRepository::MongoEapRepository() {
        ensureIndexes();
    }

    void MongoEapRepository::ensureIndexes() {

        try {
            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];

            // applicationId doubles as the manager's module name for the spawned processes, so a
            // duplicate would silently merge two applications into one process pool.
            mongocxx::options::index applicationIdOpts;
            applicationIdOpts.unique(true);
            collection.create_index(make_document(kvp("applicationId", 1)), applicationIdOpts);

            mongocxx::options::index ernOpts;
            ernOpts.unique(true);
            collection.create_index(make_document(kvp("ern", 1)), ernOpts);

        } catch (const std::exception &e) {
            log_error << "Ensure application indexes failed, error: " << e.what();
        }
    }

    Entity::EAP::Application MongoEapRepository::upsertApplication(Entity::EAP::Application &application) {

        try {
            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];

            // created is carried in toDocument(), so it is stamped here on the insert path rather
            // than through $setOnInsert - which would collide with the same field in $set.
            const auto now = std::chrono::system_clock::now();
            if (application.created.time_since_epoch().count() == 0) {
                application.created = now;
            }
            application.modified = now;

            mongocxx::options::find_one_and_update opts;
            opts.upsert(true);
            opts.return_document(mongocxx::options::return_document::k_after);

            if (auto result = collection.find_one_and_update(make_document(kvp("applicationId", application.applicationId)).view(),
                                                             make_document(kvp("$set", application.toDocument())).view(), opts)) {
                return Entity::EAP::Application::fromDocument(result->view());
            }

        } catch (const std::exception &e) {
            log_error << "Upsert application failed, error: " << e.what();
        }
        return application;
    }

    std::optional<Entity::EAP::Application> MongoEapRepository::findApplicationByApplicationId(const std::string &applicationId) const {

        try {
            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];

            if (const auto result = collection.find_one(make_document(kvp("applicationId", applicationId)).view())) {
                return Entity::EAP::Application::fromDocument(result->view());
            }

        } catch (const std::exception &e) {
            log_error << "Find application failed, error: " << e.what();
        }
        return std::nullopt;
    }

    std::optional<Entity::EAP::Application> MongoEapRepository::findApplicationByErn(const std::string &ern) const {

        try {
            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];

            if (const auto result = collection.find_one(make_document(kvp("ern", ern)).view())) {
                return Entity::EAP::Application::fromDocument(result->view());
            }

        } catch (const std::exception &e) {
            log_error << "Find application by ERN failed, error: " << e.what();
        }
        return std::nullopt;
    }

    bool MongoEapRepository::applicationExists(const std::string &applicationId) const {
        return findApplicationByApplicationId(applicationId).has_value();
    }

    std::vector<Entity::EAP::Application> MongoEapRepository::listApplications(const std::string &prefix) const {

        try {
            bsoncxx::builder::basic::document filter{};
            if (!prefix.empty()) {
                filter.append(kvp("applicationId", make_document(kvp("$regex", "^" + prefix))));
            }

            mongocxx::options::find opts;
            opts.sort(make_document(kvp("applicationId", 1)));

            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];

            std::vector<Entity::EAP::Application> result;
            for (auto cursor = collection.find(filter.extract(), opts); const auto &doc: cursor) {
                result.push_back(Entity::EAP::Application::fromDocument(doc));
            }
            return result;

        } catch (const std::exception &e) {
            log_error << "List applications failed, error: " << e.what();
            return {};
        }
    }

    long MongoEapRepository::countApplications() const {

        try {
            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];
            return static_cast<long>(collection.count_documents({}));

        } catch (const std::exception &e) {
            log_error << "Count applications failed, error: " << e.what();
            return 0;
        }
    }

    void MongoEapRepository::deleteApplication(const std::string &applicationId) {

        try {
            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];
            collection.delete_one(make_document(kvp("applicationId", applicationId)).view());

        } catch (const std::exception &e) {
            log_error << "Delete application failed, error: " << e.what();
        }
    }

}// namespace Euclid::Database
