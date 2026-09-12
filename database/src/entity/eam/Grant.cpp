// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// MongoDB includes
#include <bsoncxx/builder/basic/array.hpp>

// Euclid includes
#include <euclid/database/entity/eam/Grant.h>

namespace Euclid::Database::Entity::EAM {

    bsoncxx::document::value Grant::toDocument() const {

        bsoncxx::builder::basic::array namespacesArray;
        for (const auto &nameSpace: namespaces) namespacesArray.append(nameSpace);

        bsoncxx::builder::basic::array resourcesArray;
        for (const auto &resource: resources) resourcesArray.append(resource);

        return bsoncxx::builder::basic::make_document(
                bsoncxx::builder::basic::kvp("role", role),
                bsoncxx::builder::basic::kvp("principal", principal),
                bsoncxx::builder::basic::kvp("accountId", accountId),
                bsoncxx::builder::basic::kvp("namespaces", namespacesArray),
                bsoncxx::builder::basic::kvp("resources", resourcesArray),
                bsoncxx::builder::basic::kvp("granted", bsoncxx::types::b_date(granted)),
                bsoncxx::builder::basic::kvp("grantedBy", grantedBy));
    }

    Grant Grant::fromDocument(const std::optional<bsoncxx::document::view> &document) {
        if (!document) return {};

        Grant grant;
        for (const auto &field: *document) {
            if (const auto key = field.key(); key == "_id") grant.oid = field.get_oid().value.to_string();
            else if (key == "role") grant.role = std::string(field.get_string().value);
            else if (key == "principal") grant.principal = std::string(field.get_string().value);
            else if (key == "accountId") grant.accountId = std::string(field.get_string().value);
            else if (key == "namespaces") {
                for (const auto &element: field.get_array().value) grant.namespaces.emplace_back(element.get_string().value);
            } else if (key == "resources") {
                for (const auto &element: field.get_array().value) grant.resources.emplace_back(element.get_string().value);
            } else if (key == "granted") grant.granted = std::chrono::system_clock::time_point{field.get_date().value};
            else if (key == "grantedBy") grant.grantedBy = std::string(field.get_string().value);
        }
        return grant;
    }

}// namespace Euclid::Database::Entity::EAM
