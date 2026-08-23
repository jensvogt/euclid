//
// Created by vogje01 on 8/23/26.
//

#include <euclid/database/entity/storage/Object.h>

namespace Euclid::Database::Entity::Storage {

    bsoncxx::document::value Object::toDocument() const {

        return bsoncxx::builder::basic::make_document(
                bsoncxx::builder::basic::kvp("region", region),
                bsoncxx::builder::basic::kvp("owner", owner),
                bsoncxx::builder::basic::kvp("bucketErn", bucketErn),
                bsoncxx::builder::basic::kvp("key", key),
                bsoncxx::builder::basic::kvp("internalName", internalName),
                bsoncxx::builder::basic::kvp("ern", ern),
                bsoncxx::builder::basic::kvp("size", static_cast<int64_t>(size)),
                bsoncxx::builder::basic::kvp("status", ObjectStatusToString(status)),
                bsoncxx::builder::basic::kvp("contentType", contentType),
                bsoncxx::builder::basic::kvp("md5Sum", md5Sum));
    }

    Object Object::fromDocument(const std::optional<bsoncxx::document::view> &document) {
        if (!document) return {};

        Object object;
        for (const auto &field: *document) {
            if (const auto fieldKey = field.key(); fieldKey == "_id") object.oid = field.get_oid().value.to_string();
            else if (fieldKey == "region") object.region = std::string(field.get_string().value);
            else if (fieldKey == "owner") object.owner = std::string(field.get_string().value);
            else if (fieldKey == "bucketErn") object.bucketErn = std::string(field.get_string().value);
            else if (fieldKey == "key") object.key = std::string(field.get_string().value);
            else if (fieldKey == "internalName") object.internalName = std::string(field.get_string().value);
            else if (fieldKey == "ern") object.ern = std::string(field.get_string().value);
            else if (fieldKey == "size") object.size = field.get_int64().value;
            else if (fieldKey == "status") object.status = ObjectStatusFromString(std::string(field.get_string().value));
            else if (fieldKey == "contentType") object.contentType = std::string(field.get_string().value);
            else if (fieldKey == "md5Sum") object.md5Sum = std::string(field.get_string().value);
            else if (fieldKey == "created") object.created = system_clock::time_point{field.get_date().value};
            else if (fieldKey == "modified") object.modified = system_clock::time_point{field.get_date().value};
        }
        return object;
    }

}// namespace Euclid::Database::Entity::Storage
