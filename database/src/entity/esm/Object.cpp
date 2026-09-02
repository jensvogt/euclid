//
// Created by vogje01 on 8/23/26.
//

#include <euclid/database/entity/esm/Object.h>

namespace Euclid::Database::Entity::ESM {

    namespace {
        // See Bucket.cpp's getBsonInt for why this tolerates both int32 and int64.
        long getBsonInt(const bsoncxx::document::element &field) {
            if (field.type() == bsoncxx::type::k_int64) return field.get_int64().value;
            if (field.type() == bsoncxx::type::k_int32) return field.get_int32().value;
            return 0;
        }
    }// namespace

    bsoncxx::document::value Object::toDocument() const {

        bsoncxx::builder::basic::document attributesDoc;
        for (const auto &[name, value]: attributes) {
            attributesDoc.append(bsoncxx::builder::basic::kvp(name, value.ToDocument()));
        }

        return bsoncxx::builder::basic::make_document(
                bsoncxx::builder::basic::kvp("region", region),
                bsoncxx::builder::basic::kvp("accountId", accountId),
                bsoncxx::builder::basic::kvp("namespace", nameSpace),
                bsoncxx::builder::basic::kvp("owner", owner),
                bsoncxx::builder::basic::kvp("bucketErn", bucketErn),
                bsoncxx::builder::basic::kvp("key", key),
                bsoncxx::builder::basic::kvp("internalName", internalName),
                bsoncxx::builder::basic::kvp("ern", ern),
                bsoncxx::builder::basic::kvp("encryptionKeyErn", encryptionKeyErn),
                bsoncxx::builder::basic::kvp("size", static_cast<int64_t>(size)),
                bsoncxx::builder::basic::kvp("status", ObjectStatusToString(status)),
                bsoncxx::builder::basic::kvp("contentType", contentType),
                bsoncxx::builder::basic::kvp("md5Sum", md5Sum),
                bsoncxx::builder::basic::kvp("attributes", attributesDoc.extract()));
    }

    Object Object::fromDocument(const std::optional<bsoncxx::document::view> &document) {
        if (!document) return {};

        Object object;
        for (const auto &field: *document) {
            if (const auto fieldKey = field.key(); fieldKey == "_id") object.oid = field.get_oid().value.to_string();
            else if (fieldKey == "region") object.region = std::string(field.get_string().value);
            else if (fieldKey == "accountId") object.accountId = std::string(field.get_string().value);
            else if (fieldKey == "namespace") object.nameSpace = std::string(field.get_string().value);
            else if (fieldKey == "owner") object.owner = std::string(field.get_string().value);
            else if (fieldKey == "bucketErn") object.bucketErn = std::string(field.get_string().value);
            else if (fieldKey == "key") object.key = std::string(field.get_string().value);
            else if (fieldKey == "internalName") object.internalName = std::string(field.get_string().value);
            else if (fieldKey == "ern") object.ern = std::string(field.get_string().value);
            else if (fieldKey == "encryptionKeyErn") object.encryptionKeyErn = std::string(field.get_string().value);
            else if (fieldKey == "size") object.size = getBsonInt(field);
            else if (fieldKey == "status") object.status = ObjectStatusFromString(std::string(field.get_string().value));
            else if (fieldKey == "contentType") object.contentType = std::string(field.get_string().value);
            else if (fieldKey == "md5Sum") object.md5Sum = std::string(field.get_string().value);
            else if (fieldKey == "created") object.created = system_clock::time_point{field.get_date().value};
            else if (fieldKey == "modified") object.modified = system_clock::time_point{field.get_date().value};
            else if (fieldKey == "attributes") {
                for (const auto &attribute: field.get_document().view()) {
                    COM::Variant variant;
                    variant.FromDocument(attribute.get_document().view());
                    object.attributes[std::string(attribute.key())] = std::move(variant);
                }
            }
        }
        return object;
    }

}// namespace Euclid::Database::Entity::ESM
