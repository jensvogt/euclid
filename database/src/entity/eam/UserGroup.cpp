#include <bsoncxx/builder/basic/array.hpp>
#include <euclid/database/entity/eam/UserGroup.h>

namespace Euclid::Database::Entity::EAM {

    bsoncxx::document::value UserGroup::toDocument() const {

        bsoncxx::builder::basic::array userIdsArray;
        for (const auto &userId: userIds) userIdsArray.append(userId);

        return bsoncxx::builder::basic::make_document(
                bsoncxx::builder::basic::kvp("name", name),
                bsoncxx::builder::basic::kvp("ern", ern),
                bsoncxx::builder::basic::kvp("accountId", accountId),
                bsoncxx::builder::basic::kvp("region", region),
                bsoncxx::builder::basic::kvp("description", description),
                bsoncxx::builder::basic::kvp("userIds", userIdsArray),
                bsoncxx::builder::basic::kvp("createdAt", createdAt));
    }

    UserGroup UserGroup::fromDocument(const std::optional<bsoncxx::document::view> &document) {
        if (!document) return {};

        UserGroup group;
        for (const auto &field: *document) {
            if (const auto key = field.key(); key == "_id") group.oid = field.get_oid().value.to_string();
            else if (key == "name") group.name = std::string(field.get_string().value);
            else if (key == "ern") group.ern = std::string(field.get_string().value);
            else if (key == "accountId") group.accountId = std::string(field.get_string().value);
            else if (key == "region") group.region = std::string(field.get_string().value);
            else if (key == "description") group.description = std::string(field.get_string().value);
            else if (key == "createdAt") group.createdAt = std::string(field.get_string().value);
            else if (key == "userIds") {
                for (const auto &elem: field.get_array().value) group.userIds.emplace_back(elem.get_string().value);
            }
        }
        return group;
    }

}// namespace Euclid::Database::Entity::EAM
