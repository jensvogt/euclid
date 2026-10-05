// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <bsoncxx/builder/basic/document.hpp>
#include <euclid/database/entity/eap/Node.h>

namespace Euclid::Database::Entity::EAP {

    namespace {

        // Written as int64 and read as either, for the reason Application.cpp gives: a document
        // inserted by hand or by a migration carries int32, and get_int64() rejects that with a
        // type mismatch rather than widening - which escapes as an exception and drops the whole
        // listing rather than one field.
        long getBsonInt(const bsoncxx::document::element &field) {
            if (field.type() == bsoncxx::type::k_int64) return field.get_int64().value;
            if (field.type() == bsoncxx::type::k_int32) return field.get_int32().value;
            return 0;
        }

        std::string getBsonString(const bsoncxx::document::element &field) {
            if (field.type() == bsoncxx::type::k_string) return std::string(field.get_string().value);
            return {};
        }

        bool getBsonBool(const bsoncxx::document::element &field) {
            if (field.type() == bsoncxx::type::k_bool) return field.get_bool().value;
            return false;
        }

        system_clock::time_point getBsonDate(const bsoncxx::document::element &field) {
            if (field.type() == bsoncxx::type::k_date) return system_clock::time_point{field.get_date().value};
            return {};
        }

    }// namespace

    bsoncxx::document::value Node::toDocument() const {

        bsoncxx::builder::basic::document labelsDoc;
        for (const auto &[key, value]: labels) labelsDoc.append(bsoncxx::builder::basic::kvp(key, value));

        return bsoncxx::builder::basic::make_document(
                bsoncxx::builder::basic::kvp("name", name),
                bsoncxx::builder::basic::kvp("accountId", accountId),
                bsoncxx::builder::basic::kvp("principal", principal),
                bsoncxx::builder::basic::kvp("labels", labelsDoc.extract()),
                bsoncxx::builder::basic::kvp("cpuCount", static_cast<std::int64_t>(cpuCount)),
                bsoncxx::builder::basic::kvp("version", version),
                bsoncxx::builder::basic::kvp("os", os),
                bsoncxx::builder::basic::kvp("arch", arch),
                bsoncxx::builder::basic::kvp("loadAverage", loadAverage),
                bsoncxx::builder::basic::kvp("drained", drained),
                bsoncxx::builder::basic::kvp("lastSeen", bsoncxx::types::b_date{
                                                                 std::chrono::duration_cast<std::chrono::milliseconds>(lastSeen.time_since_epoch())}),
                // No "created": upsertNode() writes it with $setOnInsert, and MongoDB refuses an
                // update that names the same path in $set and $setOnInsert - the registration then
                // fails and the next renewal finds no node.
                bsoncxx::builder::basic::kvp("modified", bsoncxx::types::b_date{
                                                                 std::chrono::duration_cast<std::chrono::milliseconds>(modified.time_since_epoch())}));
    }

    Node Node::fromDocument(const bsoncxx::document::view &doc) {

        Node node;
        for (const auto &field: doc) {
            const auto key = field.key();
            if (key == "_id" && field.type() == bsoncxx::type::k_oid) node.oid = field.get_oid().value.to_string();
            else if (key == "name") node.name = getBsonString(field);
            else if (key == "accountId") node.accountId = getBsonString(field);
            else if (key == "principal") node.principal = getBsonString(field);
            else if (key == "cpuCount") node.cpuCount = getBsonInt(field);
            else if (key == "version") node.version = getBsonString(field);
            else if (key == "os") node.os = getBsonString(field);
            else if (key == "arch") node.arch = getBsonString(field);
            // A double on the way in, but an installation that stored a whole number reads back
            // as int32/64 - the same hazard the integer fields above have.
            else if (key == "loadAverage") node.loadAverage = field.type() == bsoncxx::type::k_double
                                                                      ? field.get_double().value
                                                                      : static_cast<double>(getBsonInt(field));
            else if (key == "drained") node.drained = getBsonBool(field);
            else if (key == "lastSeen") node.lastSeen = getBsonDate(field);
            else if (key == "created") node.created = getBsonDate(field);
            else if (key == "modified") node.modified = getBsonDate(field);
            else if (key == "labels" && field.type() == bsoncxx::type::k_document) {
                for (const auto &label: field.get_document().value) {
                    node.labels.emplace(std::string(label.key()), getBsonString(label));
                }
            }
        }
        return node;
    }

}// namespace Euclid::Database::Entity::EAP
