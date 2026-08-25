#include <euclid/database/entity/eam/Namespace.h>

namespace Euclid::Database::Entity::EAM {

    bsoncxx::document::value Namespace::toDocument() const {

        return bsoncxx::builder::basic::make_document(
                bsoncxx::builder::basic::kvp("accountId", accountId),
                bsoncxx::builder::basic::kvp("name", name),
                bsoncxx::builder::basic::kvp("ern", ern),
                bsoncxx::builder::basic::kvp("description", description),
                bsoncxx::builder::basic::kvp("created", bsoncxx::types::b_date(created)),
                bsoncxx::builder::basic::kvp("modified", bsoncxx::types::b_date(modified)));
    }

    Namespace Namespace::fromDocument(const std::optional<bsoncxx::document::view> &document) {
        if (!document) return {};

        Namespace ns;
        for (const auto &field: *document) {
            if (const auto key = field.key(); key == "_id") ns.oid = field.get_oid().value.to_string();
            else if (key == "accountId") ns.accountId = std::string(field.get_string().value);
            else if (key == "name") ns.name = std::string(field.get_string().value);
            else if (key == "ern") ns.ern = std::string(field.get_string().value);
            else if (key == "description") ns.description = std::string(field.get_string().value);
            else if (key == "created") ns.created = std::chrono::system_clock::time_point{field.get_date().value};
            else if (key == "modified") ns.modified = std::chrono::system_clock::time_point{field.get_date().value};
        }
        return ns;
    }

}// namespace Euclid::Database::Entity::EAM
