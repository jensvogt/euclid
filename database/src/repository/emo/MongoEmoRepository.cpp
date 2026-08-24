//
// Created by vogje01 on 8/18/26.
//

#include <euclid/database/repository/emo/MongoEmoRepository.h>

namespace Euclid::Database {

    MongoEmoRepository::MongoEmoRepository() {
        ensureIndexes();
    }

    void MongoEmoRepository::ensureIndexes() {

        try {
            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];

            // Covers list()'s equality filters (name/labelName/labelValue, each optional) plus its
            // always-applied timestamp sort in one index.
            collection.create_index(bsoncxx::builder::basic::make_document(
                    bsoncxx::builder::basic::kvp("name", 1),
                    bsoncxx::builder::basic::kvp("labelName", 1),
                    bsoncxx::builder::basic::kvp("labelValue", 1),
                    bsoncxx::builder::basic::kvp("timestamp", -1)));

            // Needed separately for deleteOlderThan()'s range delete - the compound index above
            // can't serve a timestamp-only filter, since it isn't the leading field.
            collection.create_index(bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp("timestamp", 1)));

        } catch (const std::exception &e) {
            log_error << "Ensure monitoring indexes failed, error: " << e.what();
        }
    }

    void MongoEmoRepository::insert(const Entity::Monitoring::MonitoringData &data) {

        try {
            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];
            collection.insert_one(data.toDocument());

        } catch (const std::exception &e) {
            log_error << "Insert monitoring data failed, error: " << e.what();
        }
    }

    std::vector<Entity::Monitoring::MonitoringData> MongoEmoRepository::list(const std::string &name, const std::string &labelName, const std::string &labelValue, const long limit) const {

        try {
            bsoncxx::builder::basic::document filter{};
            if (!name.empty()) filter.append(bsoncxx::builder::basic::kvp("name", name));
            if (!labelName.empty()) filter.append(bsoncxx::builder::basic::kvp("labelName", labelName));
            if (!labelValue.empty()) filter.append(bsoncxx::builder::basic::kvp("labelValue", labelValue));

            mongocxx::options::find opts;
            opts.sort(bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp("timestamp", -1)));
            if (limit > 0) opts.limit(limit);

            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];

            std::vector<Entity::Monitoring::MonitoringData> result;
            for (auto cursor = collection.find(filter.extract(), opts); const auto &doc: cursor) {
                result.push_back(Entity::Monitoring::MonitoringData::fromDocument(doc));
            }
            return result;

        } catch (const std::exception &e) {
            log_error << "List monitoring data failed, error: " << e.what();
            return {};
        }
    }

    void MongoEmoRepository::deleteOlderThan(const std::chrono::system_clock::time_point cutoff) {

        try {
            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];

            const auto filter = bsoncxx::builder::basic::make_document(
                    bsoncxx::builder::basic::kvp("timestamp", bsoncxx::builder::basic::make_document(
                                                         bsoncxx::builder::basic::kvp("$lt", bsoncxx::types::b_date{
                                                                                              std::chrono::duration_cast<std::chrono::milliseconds>(cutoff.time_since_epoch())}))));

            const auto result = collection.delete_many(filter.view());
            log_debug << "Pruned monitoring data, count: " << result->deleted_count();

        } catch (const std::exception &e) {
            log_error << "Prune monitoring data failed, error: " << e.what();
        }
    }

}// namespace Euclid::Database
