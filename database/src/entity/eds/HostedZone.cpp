// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// MongoDB includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>

// Euclid includes
#include <euclid/database/entity/eds/HostedZone.h>

namespace Euclid::Database::Entity::EDS {

    namespace builder = bsoncxx::builder::basic;

    bsoncxx::document::value HostedZone::toDocument() const {

        builder::document tagsDocument;
        for (const auto &[key, value]: tags) tagsDocument.append(builder::kvp(key, value));

        builder::document document;
        document.append(builder::kvp("name", name),
                        builder::kvp("ern", ern),
                        builder::kvp("accountId", accountId),
                        builder::kvp("region", region),
                        builder::kvp("namespace", nameSpace),
                        builder::kvp("ttl", static_cast<std::int64_t>(ttl)),
                        builder::kvp("description", description),
                        builder::kvp("tags", tagsDocument.extract()),
                        builder::kvp("created", bsoncxx::types::b_date{created}),
                        builder::kvp("modified", bsoncxx::types::b_date{modified}));
        return document.extract();
    }

    HostedZone HostedZone::fromDocument(const std::optional<bsoncxx::document::view> &document) {

        if (!document) return {};

        HostedZone zone;
        for (const auto &field: *document) {
            if (const auto key = field.key(); key == "_id") zone.oid = field.get_oid().value.to_string();
            else if (key == "name") zone.name = std::string(field.get_string().value);
            else if (key == "ern") zone.ern = std::string(field.get_string().value);
            else if (key == "accountId") zone.accountId = std::string(field.get_string().value);
            else if (key == "region") zone.region = std::string(field.get_string().value);
            else if (key == "namespace") zone.nameSpace = std::string(field.get_string().value);
            else if (key == "ttl") {
                if (field.type() == bsoncxx::type::k_int64) zone.ttl = field.get_int64().value;
                else if (field.type() == bsoncxx::type::k_int32) zone.ttl = field.get_int32().value;
            } else if (key == "description") zone.description = std::string(field.get_string().value);
            else if (key == "created") zone.created = std::chrono::system_clock::time_point{field.get_date().value};
            else if (key == "modified") zone.modified = std::chrono::system_clock::time_point{field.get_date().value};
            else if (key == "tags" && field.type() == bsoncxx::type::k_document) {
                for (const auto &tag: field.get_document().value) {
                    if (tag.type() == bsoncxx::type::k_string) zone.tags[std::string(tag.key())] = std::string(tag.get_string().value);
                }
            }
        }
        return zone;
    }

}// namespace Euclid::Database::Entity::EDS
