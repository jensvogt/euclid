// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 03/09/2023.
//

#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>

#include <euclid/database/entity/esm/Bucket.h>

namespace Euclid::Database::Entity::ESM {

    namespace {
        // Documents can carry "size"/"objects" as either BSON int32 or int64 - toDocument() always
        // writes int64, but manually-inserted or externally-written documents (test fixtures,
        // migrations, direct mongosh edits) can end up as plain int32, which get_int64() rejects
        // with a type-mismatch exception instead of silently narrowing. That exception used to
        // propagate out of fromDocument() and get swallowed by the repository's outer try/catch,
        // silently dropping the document from list results while leaving count queries (which don't
        // parse into an entity) unaffected - see MongoEsmRepository::listBuckets vs. countBuckets.
        long getBsonInt(const bsoncxx::document::element &field) {
            if (field.type() == bsoncxx::type::k_int64) return field.get_int64().value;
            if (field.type() == bsoncxx::type::k_int32) return field.get_int32().value;
            return 0;
        }
    }// namespace

    bsoncxx::document::value Bucket::toDocument() const {

        bsoncxx::builder::basic::document tagsDoc;
        for (const auto &[k, v]: tags) {
            tagsDoc.append(bsoncxx::builder::basic::kvp(k, v));
        }

        return bsoncxx::builder::basic::make_document(
                bsoncxx::builder::basic::kvp("region", region),
                bsoncxx::builder::basic::kvp("accountId", accountId),
                bsoncxx::builder::basic::kvp("namespace", nameSpace),
                bsoncxx::builder::basic::kvp("owner", owner),
                bsoncxx::builder::basic::kvp("name", name),
                bsoncxx::builder::basic::kvp("ern", ern),
                bsoncxx::builder::basic::kvp("encryptionKeyErn", encryptionKeyErn),
                bsoncxx::builder::basic::kvp("internal", internal),
                bsoncxx::builder::basic::kvp("priority", priority),
                bsoncxx::builder::basic::kvp("stopped", stopped),
                bsoncxx::builder::basic::kvp("lifecycleRules", [this] {
                    bsoncxx::builder::basic::array rules;
                    for (const auto &rule: lifecycleRules) {
                        rules.append(bsoncxx::builder::basic::make_document(
                                bsoncxx::builder::basic::kvp("id", rule.id),
                                bsoncxx::builder::basic::kvp("prefix", rule.prefix),
                                bsoncxx::builder::basic::kvp("archiveAfterDays", static_cast<std::int64_t>(rule.archiveAfterDays)),
                                bsoncxx::builder::basic::kvp("restoreDays", static_cast<std::int64_t>(rule.restoreDays)),
                                bsoncxx::builder::basic::kvp("enabled", rule.enabled)));
                    }
                    return rules.extract();
                }()),
                bsoncxx::builder::basic::kvp("tags", tagsDoc),
                bsoncxx::builder::basic::kvp("size", static_cast<int64_t>(size)),
                bsoncxx::builder::basic::kvp("objects", static_cast<int64_t>(objects)),
                bsoncxx::builder::basic::kvp("directories", static_cast<int64_t>(directories)));
    }

    Bucket Bucket::fromDocument(const std::optional<bsoncxx::document::view> &document) {
        if (!document) return {};

        Bucket bucket;
        for (const auto &field: *document) {
            if (const auto key = field.key(); key == "_id") bucket.oid = field.get_oid().value.to_string();
            else if (key == "region") bucket.region = std::string(field.get_string().value);
            else if (key == "accountId") bucket.accountId = std::string(field.get_string().value);
            else if (key == "namespace") bucket.nameSpace = std::string(field.get_string().value);
            else if (key == "owner") bucket.owner = std::string(field.get_string().value);
            else if (key == "name") bucket.name = std::string(field.get_string().value);
            else if (key == "ern") bucket.ern = std::string(field.get_string().value);
            else if (key == "encryptionKeyErn") bucket.encryptionKeyErn = std::string(field.get_string().value);
            else if (key == "internal") bucket.internal = field.get_bool().value;
            else if (key == "priority") bucket.priority = std::string(field.get_string().value);
            // Absent on every bucket written before buckets could be stopped, which were not.
            else if (key == "stopped" && field.type() == bsoncxx::type::k_bool) bucket.stopped = field.get_bool().value;
            // Absent on every bucket made before lifecycle rules existed, which has none.
            else if (key == "lifecycleRules" && field.type() == bsoncxx::type::k_array) {
                for (const auto &entry: field.get_array().value) {
                    if (entry.type() != bsoncxx::type::k_document) continue;
                    LifecycleRule rule;
                    for (const auto &part: entry.get_document().value) {
                        const auto name = part.key();
                        const auto asLong = [&part] {
                            return part.type() == bsoncxx::type::k_int64 ? static_cast<long>(part.get_int64().value)
                                   : part.type() == bsoncxx::type::k_int32 ? static_cast<long>(part.get_int32().value) : 0L;
                        };
                        if (name == "id" && part.type() == bsoncxx::type::k_string) rule.id = std::string(part.get_string().value);
                        else if (name == "prefix" && part.type() == bsoncxx::type::k_string) rule.prefix = std::string(part.get_string().value);
                        else if (name == "archiveAfterDays") rule.archiveAfterDays = asLong();
                        else if (name == "restoreDays") rule.restoreDays = asLong();
                        else if (name == "enabled" && part.type() == bsoncxx::type::k_bool) rule.enabled = part.get_bool().value;
                    }
                    bucket.lifecycleRules.push_back(rule);
                }
            }
            else if (key == "size") bucket.size = getBsonInt(field);
            else if (key == "objects") bucket.objects = getBsonInt(field);
            else if (key == "directories") bucket.directories = getBsonInt(field);
            else if (key == "created") bucket.created = system_clock::time_point{field.get_date().value};
            else if (key == "modified") bucket.modified = system_clock::time_point{field.get_date().value};
            else if (key == "tags") {
                for (const auto &tag: field.get_document().view()) {
                    bucket.tags[std::string(tag.key())] = std::string(tag.get_string().value);
                }
            }        }
        return bucket;
    }

}// namespace Euclid::Database::Entity::ESM