//
// Created by vogje01 on 9/10/26.
//

// MongoDB includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>

// Euclid includes
#include <euclid/database/entity/ekv/Item.h>

namespace Euclid::Database::Entity::EKV {

    namespace builder = bsoncxx::builder::basic;

    bool Item::Matches(const KeySchema &schema, const Value &value) {
        switch (schema.type) {
            case KeyType::String:
                return value.holds<std::string>();
            case KeyType::Number:
                return value.holds<std::int64_t>() || value.holds<double>();
            case KeyType::Binary:
                return value.holds<COM::Binary>();
        }
        return false;
    }

    std::optional<Item> Item::FromAttributes(const Table &table, const Map &attributes, std::string &error) {

        const auto partition = attributes.find(table.partitionKey.name);
        if (partition == attributes.end()) {
            error = "The item has no '" + table.partitionKey.name + "' attribute, which is what '" + table.name + "' is keyed on";
            return std::nullopt;
        }
        if (!Matches(table.partitionKey, partition->second)) {
            error = "'" + table.partitionKey.name + "' has to be a " + ToString(table.partitionKey.type) +
                    " in this table, and is a " + partition->second.TypeName();
            return std::nullopt;
        }

        Item item;
        item.tableName = table.name;
        item.accountId = table.accountId;
        item.nameSpace = table.nameSpace;
        item.region = table.region;
        item.partitionKey = partition->second;
        item.attributes = attributes;

        if (table.sortKey.has_value()) {
            const auto sort = attributes.find(table.sortKey->name);
            if (sort == attributes.end()) {
                error = "The item has no '" + table.sortKey->name + "' attribute, which is what '" + table.name + "' is sorted by";
                return std::nullopt;
            }
            if (!Matches(*table.sortKey, sort->second)) {
                error = "'" + table.sortKey->name + "' has to be a " + ToString(table.sortKey->type) +
                        " in this table, and is a " + sort->second.TypeName();
                return std::nullopt;
            }
            item.sortKey = sort->second;
        }
        return item;
    }

    bsoncxx::document::value Item::toDocument() const {

        builder::document attributesDocument;
        for (const auto &[name, value]: attributes) value.AppendTo(attributesDocument, name);

        builder::document document;
        document.append(builder::kvp("tableName", tableName),
                        builder::kvp("accountId", accountId),
                        builder::kvp("namespace", nameSpace),
                        builder::kvp("region", region));

        // The key values in their own types, so the database compares them as numbers where they
        // are numbers - which is the whole point of declaring a sort key's type, and what a
        // stringified key could never give: "10" sorts before "9".
        partitionKey.AppendTo(document, "pk");
        if (sortKey.has_value()) sortKey->AppendTo(document, "sk");

        // Without the timestamps: a write is an upsert, and the repository sets "created" only
        // when there was nothing there and "modified" on every write. Putting them here as well
        // would have the two halves of that update argue over the same field, which the database
        // refuses outright.
        document.append(builder::kvp("item", attributesDocument.extract()));
        return document.extract();
    }

    Item Item::fromDocument(const std::optional<bsoncxx::document::view> &document) {

        if (!document) return {};

        Item item;
        for (const auto &field: *document) {
            if (const auto key = field.key(); key == "_id") item.oid = field.get_oid().value.to_string();
            else if (key == "tableName") item.tableName = std::string(field.get_string().value);
            else if (key == "accountId") item.accountId = std::string(field.get_string().value);
            else if (key == "namespace") item.nameSpace = std::string(field.get_string().value);
            else if (key == "region") item.region = std::string(field.get_string().value);
            else if (key == "pk") item.partitionKey = Value::FromBson(field.get_value());
            else if (key == "sk") item.sortKey = Value::FromBson(field.get_value());
            else if (key == "item") item.attributes = Value::MapFromBson(field.get_document().value);
            else if (key == "created") item.created = std::chrono::system_clock::time_point{field.get_date().value};
            else if (key == "modified") item.modified = std::chrono::system_clock::time_point{field.get_date().value};
        }
        return item;
    }

}// namespace Euclid::Database::Entity::EKV
