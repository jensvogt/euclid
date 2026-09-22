// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Euclid includes
#include <euclid/database/entity/eam/AuthorizationGap.h>

namespace Euclid::Database::Entity::EAM {

    bsoncxx::document::value AuthorizationGap::toDocument() const {

        return bsoncxx::builder::basic::make_document(
                bsoncxx::builder::basic::kvp("userId", userId),
                bsoncxx::builder::basic::kvp("accountId", accountId),
                bsoncxx::builder::basic::kvp("namespace", nameSpace),
                bsoncxx::builder::basic::kvp("target", target),
                bsoncxx::builder::basic::kvp("action", action),
                bsoncxx::builder::basic::kvp("reason", reason),
                bsoncxx::builder::basic::kvp("count", static_cast<int64_t>(count)),
                bsoncxx::builder::basic::kvp("firstSeen", bsoncxx::types::b_date(firstSeen)),
                bsoncxx::builder::basic::kvp("lastSeen", bsoncxx::types::b_date(lastSeen)));
    }

    AuthorizationGap AuthorizationGap::fromDocument(const std::optional<bsoncxx::document::view> &document) {
        if (!document) return {};

        AuthorizationGap gap;
        for (const auto &field: *document) {
            if (const auto key = field.key(); key == "_id") gap.oid = field.get_oid().value.to_string();
            else if (key == "userId") gap.userId = std::string(field.get_string().value);
            else if (key == "accountId") gap.accountId = std::string(field.get_string().value);
            else if (key == "namespace") gap.nameSpace = std::string(field.get_string().value);
            else if (key == "target") gap.target = std::string(field.get_string().value);
            else if (key == "action") gap.action = std::string(field.get_string().value);
            else if (key == "reason") gap.reason = std::string(field.get_string().value);
            else if (key == "count") gap.count = field.type() == bsoncxx::type::k_int64 ? static_cast<long>(field.get_int64().value)
                                                                                       : static_cast<long>(field.get_int32().value);
            else if (key == "firstSeen") gap.firstSeen = std::chrono::system_clock::time_point{field.get_date().value};
            else if (key == "lastSeen") gap.lastSeen = std::chrono::system_clock::time_point{field.get_date().value};
        }
        return gap;
    }

}// namespace Euclid::Database::Entity::EAM
