// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/10/26.
//

// C++ includes
#include <algorithm>

// MongoDB includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>

// Euclid includes
#include <euclid/database/entity/ekv/Table.h>

namespace Euclid::Database::Entity::EKV {

    namespace builder = bsoncxx::builder::basic;

    std::string ToString(const KeyType type) {
        switch (type) {
            case KeyType::Number:
                return "number";
            case KeyType::Binary:
                return "binary";
            case KeyType::String:
            default:
                return "string";
        }
    }

    std::optional<KeyType> KeyTypeFromString(const std::string &name) {

        std::string lowered = name;
        std::ranges::transform(lowered, lowered.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (lowered == "string" || lowered == "s") return KeyType::String;
        if (lowered == "number" || lowered == "n") return KeyType::Number;
        if (lowered == "binary" || lowered == "b") return KeyType::Binary;
        return std::nullopt;
    }

    bsoncxx::document::value Table::toDocument() const {

        builder::document document;
        document.append(builder::kvp("name", name),
                        builder::kvp("ern", ern),
                        builder::kvp("accountId", accountId),
                        builder::kvp("region", region),
                        builder::kvp("namespace", nameSpace),
                        builder::kvp("partitionKeyName", partitionKey.name),
                        builder::kvp("partitionKeyType", ToString(partitionKey.type)),
                        builder::kvp("created", bsoncxx::types::b_date{created}),
                        builder::kvp("modified", bsoncxx::types::b_date{modified}));

        // Absent rather than empty when there is none: "this table has no sort key" and "this
        // table's sort key is called nothing" should not be the same document.
        if (sortKey.has_value()) {
            document.append(builder::kvp("sortKeyName", sortKey->name),
                            builder::kvp("sortKeyType", ToString(sortKey->type)));
        }
        return document.extract();
    }

    Table Table::fromDocument(const std::optional<bsoncxx::document::view> &document) {

        if (!document) return {};

        Table table;
        std::string sortKeyName, sortKeyType;

        for (const auto &field: *document) {
            if (const auto key = field.key(); key == "_id") table.oid = field.get_oid().value.to_string();
            else if (key == "name") table.name = std::string(field.get_string().value);
            else if (key == "ern") table.ern = std::string(field.get_string().value);
            else if (key == "accountId") table.accountId = std::string(field.get_string().value);
            else if (key == "region") table.region = std::string(field.get_string().value);
            else if (key == "namespace") table.nameSpace = std::string(field.get_string().value);
            else if (key == "partitionKeyName") table.partitionKey.name = std::string(field.get_string().value);
            else if (key == "partitionKeyType") table.partitionKey.type = KeyTypeFromString(std::string(field.get_string().value)).value_or(KeyType::String);
            else if (key == "sortKeyName") sortKeyName = std::string(field.get_string().value);
            else if (key == "sortKeyType") sortKeyType = std::string(field.get_string().value);
            else if (key == "created") table.created = std::chrono::system_clock::time_point{field.get_date().value};
            else if (key == "modified") table.modified = std::chrono::system_clock::time_point{field.get_date().value};
        }

        if (!sortKeyName.empty()) {
            table.sortKey = KeySchema{.name = sortKeyName, .type = KeyTypeFromString(sortKeyType).value_or(KeyType::String)};
        }
        return table;
    }

}// namespace Euclid::Database::Entity::EKV
