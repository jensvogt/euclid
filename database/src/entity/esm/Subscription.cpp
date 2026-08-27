//
// Created by vogje01 on 8/27/26.
//

#include <euclid/database/entity/esm/Subscription.h>

namespace Euclid::Database::Entity::ESM {

    bsoncxx::document::value Subscription::toDocument() const {

        return bsoncxx::builder::basic::make_document(
                bsoncxx::builder::basic::kvp("region", region),
                bsoncxx::builder::basic::kvp("accountId", accountId),
                bsoncxx::builder::basic::kvp("namespace", nameSpace),
                bsoncxx::builder::basic::kvp("owner", owner),
                bsoncxx::builder::basic::kvp("ern", ern),
                bsoncxx::builder::basic::kvp("sourceErn", sourceErn),
                bsoncxx::builder::basic::kvp("type", type),
                bsoncxx::builder::basic::kvp("targetErn", targetErn));
    }

    Subscription Subscription::fromDocument(const std::optional<bsoncxx::document::view> &document) {
        if (!document) return {};

        Subscription subscription;
        for (const auto &field: *document) {
            if (const auto key = field.key(); key == "_id") subscription.oid = field.get_oid().value.to_string();
            else if (key == "region") subscription.region = std::string(field.get_string().value);
            else if (key == "accountId") subscription.accountId = std::string(field.get_string().value);
            else if (key == "namespace") subscription.nameSpace = std::string(field.get_string().value);
            else if (key == "owner") subscription.owner = std::string(field.get_string().value);
            else if (key == "ern") subscription.ern = std::string(field.get_string().value);
            else if (key == "sourceErn") subscription.sourceErn = std::string(field.get_string().value);
            else if (key == "type") subscription.type = std::string(field.get_string().value);
            else if (key == "targetErn") subscription.targetErn = std::string(field.get_string().value);
            else if (key == "created") subscription.created = system_clock::time_point{field.get_date().value};
            else if (key == "modified") subscription.modified = system_clock::time_point{field.get_date().value};
        }
        return subscription;
    }

}// namespace Euclid::Database::Entity::ESM
