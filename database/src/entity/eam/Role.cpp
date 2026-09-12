// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// MongoDB includes
#include <bsoncxx/builder/basic/array.hpp>

// Euclid includes
#include <euclid/database/entity/eam/Role.h>

namespace Euclid::Database::Entity::EAM {

    bsoncxx::document::value Role::toDocument() const {

        bsoncxx::builder::basic::array permissionsArray;
        for (const auto &permission: permissions) permissionsArray.append(permission);

        return bsoncxx::builder::basic::make_document(
                bsoncxx::builder::basic::kvp("name", name),
                bsoncxx::builder::basic::kvp("ern", ern),
                bsoncxx::builder::basic::kvp("accountId", accountId),
                bsoncxx::builder::basic::kvp("region", region),
                bsoncxx::builder::basic::kvp("description", description),
                bsoncxx::builder::basic::kvp("permissions", permissionsArray),
                bsoncxx::builder::basic::kvp("created", bsoncxx::types::b_date(created)),
                bsoncxx::builder::basic::kvp("modified", bsoncxx::types::b_date(modified)));
    }

    Role Role::fromDocument(const std::optional<bsoncxx::document::view> &document) {
        if (!document) return {};

        Role role;
        for (const auto &field: *document) {
            if (const auto key = field.key(); key == "_id") role.oid = field.get_oid().value.to_string();
            else if (key == "name") role.name = std::string(field.get_string().value);
            else if (key == "ern") role.ern = std::string(field.get_string().value);
            else if (key == "accountId") role.accountId = std::string(field.get_string().value);
            else if (key == "region") role.region = std::string(field.get_string().value);
            else if (key == "description") role.description = std::string(field.get_string().value);
            else if (key == "permissions") {
                for (const auto &element: field.get_array().value) role.permissions.emplace_back(element.get_string().value);
            } else if (key == "created") role.created = std::chrono::system_clock::time_point{field.get_date().value};
            else if (key == "modified") role.modified = std::chrono::system_clock::time_point{field.get_date().value};
        }
        return role;
    }

}// namespace Euclid::Database::Entity::EAM
