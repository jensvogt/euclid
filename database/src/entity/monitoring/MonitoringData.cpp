//
// Created by vogje01 on 8/18/26.
//

#include <euclid/database/entity/monitoring/MonitoringData.h>

namespace Euclid::Database::Entity::Monitoring {

    bsoncxx::document::value MonitoringData::toDocument() const {

        return bsoncxx::builder::basic::make_document(
                bsoncxx::builder::basic::kvp("name", name),
                bsoncxx::builder::basic::kvp("labelName", labelName),
                bsoncxx::builder::basic::kvp("labelValue", labelValue),
                bsoncxx::builder::basic::kvp("value", value),
                bsoncxx::builder::basic::kvp("timestamp", bsoncxx::types::b_date{
                                                     std::chrono::duration_cast<std::chrono::milliseconds>(timestamp.time_since_epoch())}));
    }

    MonitoringData MonitoringData::fromDocument(const std::optional<bsoncxx::document::view> &doc) {
        if (!doc) return {};

        MonitoringData data;
        for (const auto &field: *doc) {
            if (const auto key = field.key(); key == "name") data.name = std::string(field.get_string().value);
            else if (key == "labelName") data.labelName = std::string(field.get_string().value);
            else if (key == "labelValue") data.labelValue = std::string(field.get_string().value);
            else if (key == "value") data.value = field.get_double().value;
            else if (key == "timestamp") data.timestamp = std::chrono::system_clock::time_point{field.get_date().value};
            else if (key == "_id") data.oid = field.get_oid().value.to_string();
        }
        return data;
    }

}// namespace Euclid::Database::Entity::Monitoring
