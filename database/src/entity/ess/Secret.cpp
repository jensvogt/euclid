// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/8/26.
//

#include <euclid/database/entity/ess/Secret.h>

namespace Euclid::Database::Entity::ESS {

    namespace {
        long getBsonInt(const bsoncxx::document::element &field) {
            if (field.type() == bsoncxx::type::k_int64) return field.get_int64().value;
            if (field.type() == bsoncxx::type::k_int32) return field.get_int32().value;
            return 0;
        }
    }// namespace

    bsoncxx::document::value Secret::toDocument() const {

        bsoncxx::builder::basic::document tagsDoc;
        for (const auto &[k, v]: tags) {
            tagsDoc.append(bsoncxx::builder::basic::kvp(k, v));
        }

        return bsoncxx::builder::basic::make_document(
                bsoncxx::builder::basic::kvp("region", region),
                bsoncxx::builder::basic::kvp("accountId", accountId),
                bsoncxx::builder::basic::kvp("namespace", nameSpace),
                bsoncxx::builder::basic::kvp("ern", ern),
                bsoncxx::builder::basic::kvp("name", name),
                bsoncxx::builder::basic::kvp("description", description),
                // Ciphertext - see Secret::value. Nothing writes a plaintext value here.
                bsoncxx::builder::basic::kvp("value", value),
                bsoncxx::builder::basic::kvp("encryptionKeyErn", encryptionKeyErn),
                bsoncxx::builder::basic::kvp("version", static_cast<int64_t>(version)),
                bsoncxx::builder::basic::kvp("rotated", bsoncxx::types::b_date{std::chrono::duration_cast<std::chrono::milliseconds>(rotated.time_since_epoch())}),
                bsoncxx::builder::basic::kvp("tags", tagsDoc.extract()));
    }

    Secret Secret::fromDocument(const std::optional<bsoncxx::document::view> &document) {
        if (!document) return {};

        Secret secret;
        for (const auto &field: *document) {
            if (const auto key = field.key(); key == "_id") secret.oid = field.get_oid().value.to_string();
            else if (key == "region") secret.region = std::string(field.get_string().value);
            else if (key == "accountId") secret.accountId = std::string(field.get_string().value);
            else if (key == "namespace") secret.nameSpace = std::string(field.get_string().value);
            else if (key == "ern") secret.ern = std::string(field.get_string().value);
            else if (key == "name") secret.name = std::string(field.get_string().value);
            else if (key == "description") secret.description = std::string(field.get_string().value);
            else if (key == "value") secret.value = std::string(field.get_string().value);
            else if (key == "encryptionKeyErn") secret.encryptionKeyErn = std::string(field.get_string().value);
            else if (key == "version") secret.version = getBsonInt(field);
            else if (key == "rotated") secret.rotated = system_clock::time_point{field.get_date().value};
            else if (key == "created") secret.created = system_clock::time_point{field.get_date().value};
            else if (key == "modified") secret.modified = system_clock::time_point{field.get_date().value};
            else if (key == "tags") {
                for (const auto &tag: field.get_document().view()) {
                    secret.tags[std::string(tag.key())] = std::string(tag.get_string().value);
                }
            }
        }
        return secret;
    }

}// namespace Euclid::Database::Entity::ESS
