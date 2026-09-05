//
// Created by vogje01 on 01/06/2023.
//

#include <euclid/database/entity/eqs/Message.h>

// MongoDB includes
#include <bsoncxx/json.hpp>

// Euclid includes
#include <euclid/core/CryptoUtils.h>

namespace Euclid::Database::Entity::EQS {

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
                bsoncxx::builder::basic::kvp("queueErn", queueErn),
                bsoncxx::builder::basic::kvp("sourceQueueErn", sourceQueueErn),
                bsoncxx::builder::basic::kvp("body", body),
                bsoncxx::builder::basic::kvp("status", MessageStatusToString(status)),
                bsoncxx::builder::basic::kvp("priority", MessagePriorityToString(priority)),
                bsoncxx::builder::basic::kvp("receivedCount", static_cast<int32_t>(receivedCount)),
                bsoncxx::builder::basic::kvp("size", static_cast<int64_t>(size)),
                bsoncxx::builder::basic::kvp("visibilityTimeout", static_cast<int32_t>(visibilityTimeout)),
                bsoncxx::builder::basic::kvp("messageId", messageId),
                bsoncxx::builder::basic::kvp("receiptHandle", receiptHandle),
                bsoncxx::builder::basic::kvp("contentType", contentType),
                bsoncxx::builder::basic::kvp("reset", bsoncxx::types::b_date(reset)),
                bsoncxx::builder::basic::kvp("delayUntil", bsoncxx::types::b_date(delayUntil)),
                bsoncxx::builder::basic::kvp("lastReceived", bsoncxx::types::b_date(lastReceived)),
                bsoncxx::builder::basic::kvp("attributes", attrsDoc.extract()));
    }

    void Message::FromDocument(const std::optional<bsoncxx::document::view> &document) {
        if (!document) return;
        for (const auto &field: *document) {
            if (const auto key = field.key(); key == "_id") oid = field.get_oid().value.to_string();
            else if (key == "ern") ern = std::string(field.get_string().value);
            else if (key == "queueErn") queueErn = std::string(field.get_string().value);
            else if (key == "sourceQueueErn") sourceQueueErn = std::string(field.get_string().value);
            else if (key == "body") body = std::string(field.get_string().value);
            else if (key == "status") status = MessageStatusFromString(std::string(field.get_string().value));
            else if (key == "priority") priority = MessagePriorityFromString(std::string(field.get_string().value));
            else if (key == "receivedCount") receivedCount = getBsonInt(field);
            else if (key == "size") size = getBsonInt(field);
            else if (key == "visibilityTimeout") visibilityTimeout = static_cast<int>(getBsonInt(field));
            else if (key == "messageId") messageId = std::string(field.get_string().value);
            else if (key == "receiptHandle") receiptHandle = std::string(field.get_string().value);
            else if (key == "contentType") contentType = std::string(field.get_string().value);
            else if (key == "reset") reset = system_clock::time_point{field.get_date().value};
            else if (key == "delayUntil") delayUntil = system_clock::time_point{field.get_date().value};
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


}// namespace Euclid::Database::Entity::SQS