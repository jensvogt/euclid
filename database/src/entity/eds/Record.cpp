// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// MongoDB includes
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>

// Euclid includes
#include <euclid/database/entity/eds/Record.h>

namespace Euclid::Database::Entity::EDS {

    namespace builder = bsoncxx::builder::basic;

    bsoncxx::document::value Record::toDocument() const {

        builder::array valuesArray;
        for (const auto &value: values) valuesArray.append(value);

        builder::document document;
        document.append(builder::kvp("accountId", accountId),
                        builder::kvp("zoneName", zoneName),
                        builder::kvp("name", name),
                        builder::kvp("type", type),
                        builder::kvp("ttl", static_cast<std::int64_t>(ttl)),
                        builder::kvp("values", valuesArray.extract()),
                        builder::kvp("created", bsoncxx::types::b_date{created}),
                        builder::kvp("modified", bsoncxx::types::b_date{modified}));
        return document.extract();
    }

    Record Record::fromDocument(const std::optional<bsoncxx::document::view> &document) {

        if (!document) return {};

        Record record;
        for (const auto &field: *document) {
            if (const auto key = field.key(); key == "_id") record.oid = field.get_oid().value.to_string();
            else if (key == "accountId") record.accountId = std::string(field.get_string().value);
            else if (key == "zoneName") record.zoneName = std::string(field.get_string().value);
            else if (key == "name") record.name = std::string(field.get_string().value);
            else if (key == "type") record.type = std::string(field.get_string().value);
            else if (key == "ttl") {
                if (field.type() == bsoncxx::type::k_int64) record.ttl = field.get_int64().value;
                else if (field.type() == bsoncxx::type::k_int32) record.ttl = field.get_int32().value;
            } else if (key == "values" && field.type() == bsoncxx::type::k_array) {
                for (const auto &value: field.get_array().value) {
                    if (value.type() == bsoncxx::type::k_string) record.values.emplace_back(value.get_string().value);
                }
            } else if (key == "created") record.created = std::chrono::system_clock::time_point{field.get_date().value};
            else if (key == "modified") record.modified = std::chrono::system_clock::time_point{field.get_date().value};
        }
        return record;
    }

}// namespace Euclid::Database::Entity::EDS
