//
// Created by vogje01 on 03/09/2023.
//

#include <euclid/database/entity/ekm/Key.h>

namespace Euclid::Database::Entity::EKM {

    namespace {
        long getBsonInt(const bsoncxx::document::element &field) {
            if (field.type() == bsoncxx::type::k_int64) return field.get_int64().value;
            if (field.type() == bsoncxx::type::k_int32) return field.get_int32().value;
            return 0;
        }
    }

    bsoncxx::document::value Key::toDocument() const {

        bsoncxx::builder::basic::document tagsDoc;
        for (const auto &[k, v]: tags) {
            tagsDoc.append(bsoncxx::builder::basic::kvp(k, v));
        }

        return bsoncxx::builder::basic::make_document(
                bsoncxx::builder::basic::kvp("region", region),
                bsoncxx::builder::basic::kvp("accountId", accountId),
                bsoncxx::builder::basic::kvp("namespace", nameSpace),
                bsoncxx::builder::basic::kvp("algorithm", algorithm),
                bsoncxx::builder::basic::kvp("length", length),
                bsoncxx::builder::basic::kvp("tags", tagsDoc.extract()));
    }

    Key Key::fromDocument(const std::optional<bsoncxx::document::view> &document) {
        if (!document) return {};

        Key key;
        for (const auto &field: *document) {
            if (const auto k = field.key(); k == "_id") key.oid = field.get_oid().value.to_string();
            else if (k == "region") key.region = std::string(field.get_string().value);
            else if (k == "accountId") key.accountId = std::string(field.get_string().value);
            else if (k == "namespace") key.nameSpace = std::string(field.get_string().value);
            else if (k == "algorithm") key.algorithm = std::string(field.get_string().value);
            else if (k == "length") key.length = getBsonInt(field);
            else if (k == "created") key.created = system_clock::time_point{field.get_date().value};
            else if (k == "modified") key.modified = system_clock::time_point{field.get_date().value};
            else if (k == "tags") {
                for (const auto &tag: field.get_document().view()) {
                    key.tags[std::string(tag.key())] = std::string(tag.get_string().value);
                }
            }
        }
        return key;
    }

}// namespace Euclid::Database::Entity::SQS