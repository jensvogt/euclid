//
// Created by vogje01 on 01/06/2023.
//

#include <euclid/database/entity/ens/Message.h>

namespace Euclid::Database::Entity::ENS {

    namespace {
        long getBsonInt(const bsoncxx::document::element &field) {
            if (field.type() == bsoncxx::type::k_int64) return field.get_int64().value;
            if (field.type() == bsoncxx::type::k_int32) return field.get_int32().value;
            return 0;
        }
    }

    bsoncxx::document::value Message::ToDocument() const {

        bsoncxx::builder::basic::document attrsDoc;
        for (const auto &[k, v]: attributes) {
            attrsDoc.append(bsoncxx::builder::basic::kvp(k, v.ToDocument()));
        }

        return bsoncxx::builder::basic::make_document(
                bsoncxx::builder::basic::kvp("ern", ern),
                bsoncxx::builder::basic::kvp("topicErn", topicErn),
                bsoncxx::builder::basic::kvp("body", body),
                bsoncxx::builder::basic::kvp("size", static_cast<int64_t>(size)),
                bsoncxx::builder::basic::kvp("messageId", messageId),
                bsoncxx::builder::basic::kvp("receiptHandle", receiptHandle),
                bsoncxx::builder::basic::kvp("md5Body", md5Body),
                bsoncxx::builder::basic::kvp("md5Attributes", md5Attributes),
                bsoncxx::builder::basic::kvp("contentType", contentType),
                bsoncxx::builder::basic::kvp("lastReceived", bsoncxx::types::b_date(lastReceived)),
                bsoncxx::builder::basic::kvp("attributes", attrsDoc.extract()),
                bsoncxx::builder::basic::kvp("created", bsoncxx::types::b_date(created)),
                bsoncxx::builder::basic::kvp("modified", bsoncxx::types::b_date(modified)));
    }

    void Message::FromDocument(const std::optional<bsoncxx::document::view> &document) {
        if (!document) return;
        for (const auto &field: *document) {
            if (const auto key = field.key(); key == "_id") oid = field.get_oid().value.to_string();
            else if (key == "ern") ern = std::string(field.get_string().value);
            else if (key == "topicErn") topicErn = std::string(field.get_string().value);
            else if (key == "body") body = std::string(field.get_string().value);
            else if (key == "size") size = getBsonInt(field);
            else if (key == "messageId") messageId = std::string(field.get_string().value);
            else if (key == "receiptHandle") receiptHandle = std::string(field.get_string().value);
            else if (key == "md5Body") md5Body = std::string(field.get_string().value);
            else if (key == "md5Attributes") md5Attributes = std::string(field.get_string().value);
            else if (key == "contentType") contentType = std::string(field.get_string().value);
            else if (key == "lastReceived") lastReceived = system_clock::time_point{field.get_date().value};
            else if (key == "created") created = system_clock::time_point{field.get_date().value};
            else if (key == "modified") modified = system_clock::time_point{field.get_date().value};
            else if (key == "attributes") {
                for (const auto &attr: field.get_document().view()) {
                    COM::Variant variant;
                    variant.FromDocument(attr.get_document().view());
                    attributes[std::string(attr.key())] = std::move(variant);
                }
            }
        }
    }

    std::string Message::ComputeAttributesMd5(const std::map<std::string, COM::Variant> &attributes) {
        bsoncxx::builder::basic::document attrsDoc;
        for (const auto &[k, v]: attributes) {
            attrsDoc.append(bsoncxx::builder::basic::kvp(k, v.ToDocument()));
        }
        return Core::CryptoUtils::md5Sum(bsoncxx::to_json(attrsDoc.view()));
    }

}// namespace Euclid::Database::Entity::SQS