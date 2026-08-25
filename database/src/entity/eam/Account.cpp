#include <euclid/database/entity/eam/Account.h>

namespace Euclid::Database::Entity::EAM {

    bsoncxx::document::value Account::toDocument() const {

        return bsoncxx::builder::basic::make_document(
                bsoncxx::builder::basic::kvp("accountId", accountId),
                bsoncxx::builder::basic::kvp("name", name),
                bsoncxx::builder::basic::kvp("ern", ern),
                bsoncxx::builder::basic::kvp("description", description),
                bsoncxx::builder::basic::kvp("created", bsoncxx::types::b_date(created)),
                bsoncxx::builder::basic::kvp("modified", bsoncxx::types::b_date(modified)));
    }

    Account Account::fromDocument(const std::optional<bsoncxx::document::view> &document) {
        if (!document) return {};

        Account account;
        for (const auto &field: *document) {
            if (const auto key = field.key(); key == "_id") account.oid = field.get_oid().value.to_string();
            else if (key == "accountId") account.accountId = std::string(field.get_string().value);
            else if (key == "name") account.name = std::string(field.get_string().value);
            else if (key == "ern") account.ern = std::string(field.get_string().value);
            else if (key == "description") account.description = std::string(field.get_string().value);
            else if (key == "created") account.created = std::chrono::system_clock::time_point{field.get_date().value};
            else if (key == "modified") account.modified = std::chrono::system_clock::time_point{field.get_date().value};
        }
        return account;
    }

}// namespace Euclid::Database::Entity::EAM
