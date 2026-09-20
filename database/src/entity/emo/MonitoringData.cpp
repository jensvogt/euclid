// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/18/26.
//

#include <euclid/database/entity/emo/MonitoringData.h>

namespace Euclid::Database::Entity::Monitoring {

    bsoncxx::document::value MonitoringData::toDocument() const {

        bsoncxx::builder::basic::document labelDocument{};
        for (const auto &[labelName, labelValue]: labels) {
            labelDocument.append(bsoncxx::builder::basic::kvp(labelName, labelValue));
        }

        return bsoncxx::builder::basic::make_document(
                bsoncxx::builder::basic::kvp("name", name),
                bsoncxx::builder::basic::kvp("labels", labelDocument.extract()),
                // Denormalised deliberately, and the one field a query may not skip: it is what
                // the unique index and the rollup's $merge identify a series by. See LabelKey().
                bsoncxx::builder::basic::kvp("labelKey", labelKey()),
                bsoncxx::builder::basic::kvp("value", value),
                bsoncxx::builder::basic::kvp("minValue", minValue),
                bsoncxx::builder::basic::kvp("maxValue", maxValue),
                bsoncxx::builder::basic::kvp("samples", static_cast<std::int64_t>(samples)),
                bsoncxx::builder::basic::kvp("type", MetricTypeToString(type)),
                bsoncxx::builder::basic::kvp("resolution", ResolutionToString(resolution)),
                bsoncxx::builder::basic::kvp("timestamp", bsoncxx::types::b_date{
                                                     std::chrono::duration_cast<std::chrono::milliseconds>(timestamp.time_since_epoch())}),
                bsoncxx::builder::basic::kvp("expiresAt", bsoncxx::types::b_date{
                                                     std::chrono::duration_cast<std::chrono::milliseconds>(expiresAt.time_since_epoch())}));
    }

    MonitoringData MonitoringData::fromDocument(const std::optional<bsoncxx::document::view> &doc) {
        if (!doc) return {};

        MonitoringData data;
        std::string legacyName, legacyValue;
        for (const auto &field: *doc) {
            if (const auto key = field.key(); key == "name") data.name = std::string(field.get_string().value);
            else if (key == "labels" && field.type() == bsoncxx::type::k_document) {
                for (const auto &label: field.get_document().value) {
                    if (label.type() == bsoncxx::type::k_string)
                        data.labels[std::string(label.key())] = std::string(label.get_string().value);
                }
            }
            // Rows written before the map. Read into it rather than migrated: a data point lives
            // as long as its tier's retention and is then gone by itself, so the two shapes only
            // have to coexist for that long.
            else if (key == "labelName" && field.type() == bsoncxx::type::k_string) legacyName = std::string(field.get_string().value);
            else if (key == "labelValue" && field.type() == bsoncxx::type::k_string) legacyValue = std::string(field.get_string().value);
            else if (key == "value") data.value = field.get_double().value;
            else if (key == "minValue") data.minValue = field.get_double().value;
            else if (key == "maxValue") data.maxValue = field.get_double().value;
            else if (key == "samples") data.samples = static_cast<long>(field.get_int64().value);
            else if (key == "type") data.type = MetricTypeFromString(std::string(field.get_string().value));
            else if (key == "resolution") data.resolution = ResolutionFromString(std::string(field.get_string().value));
            else if (key == "timestamp") data.timestamp = std::chrono::system_clock::time_point{field.get_date().value};
            else if (key == "expiresAt") data.expiresAt = std::chrono::system_clock::time_point{field.get_date().value};
            else if (key == "_id") data.oid = field.get_oid().value.to_string();
        }

        if (data.labels.empty() && !legacyName.empty()) {
            data.labels[legacyName] = legacyValue;
        }
        return data;
    }

}// namespace Euclid::Database::Entity::Monitoring
