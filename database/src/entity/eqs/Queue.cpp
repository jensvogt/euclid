//
// Created by vogje01 on 03/09/2023.
//

#include <euclid/database/entity/eqs/Queue.h>

namespace Euclid::Database::Entity::EQS {

    namespace {
        long getBsonInt(const bsoncxx::document::element &field) {
            if (field.type() == bsoncxx::type::k_int64) return field.get_int64().value;
            if (field.type() == bsoncxx::type::k_int32) return field.get_int32().value;
            return 0;
        }
    }

    bsoncxx::document::value Queue::toDocument() const {

        bsoncxx::builder::basic::document tagsDoc;
        for (const auto &[k, v]: tags) {
            tagsDoc.append(bsoncxx::builder::basic::kvp(k, v));
        }

        // bsoncxx::builder::basic::document defaultMessageAttributesDoc;
        // for (const auto &[k, v]: defaultMessageAttributes) {
        //     defaultMessageAttributesDoc.append(bsoncxx::builder::basic::kvp(k, v.ToDocument()));
        // }

        return bsoncxx::builder::basic::make_document(
                bsoncxx::builder::basic::kvp("region", region),
                bsoncxx::builder::basic::kvp("accountId", accountId),
                bsoncxx::builder::basic::kvp("namespace", namespaceName),
                bsoncxx::builder::basic::kvp("owner", owner),
                bsoncxx::builder::basic::kvp("name", name),
                bsoncxx::builder::basic::kvp("ern", ern),
                bsoncxx::builder::basic::kvp("size", static_cast<int64_t>(size)),
                bsoncxx::builder::basic::kvp("delay", static_cast<int64_t>(delay)),
                bsoncxx::builder::basic::kvp("available", static_cast<int64_t>(available)),
                bsoncxx::builder::basic::kvp("delayed", static_cast<int64_t>(delayed)),
                bsoncxx::builder::basic::kvp("invisible", static_cast<int64_t>(invisible)),
                bsoncxx::builder::basic::kvp("visibility", static_cast<int64_t>(visibility)),
                bsoncxx::builder::basic::kvp("maxMessageLength", static_cast<int64_t>(maxMessageLength)),
                bsoncxx::builder::basic::kvp("maxReceiveCount", static_cast<int64_t>(maxReceiveCount)),
                bsoncxx::builder::basic::kvp("deadLetterQueueErn", deadLetterQueueErn),
                bsoncxx::builder::basic::kvp("tags", tagsDoc.extract()));
        // bsoncxx::builder::basic::kvp("defaultMessageAttributes", defaultMessageAttributesDoc.extract()));
    }

    Queue Queue::fromDocument(const std::optional<bsoncxx::document::view> &document) {
        if (!document) return {};

        Queue queue;
        for (const auto &field: *document) {
            if (const auto key = field.key(); key == "_id") queue.oid = field.get_oid().value.to_string();
            else if (key == "region") queue.region = std::string(field.get_string().value);
            else if (key == "accountId") queue.accountId = std::string(field.get_string().value);
            else if (key == "namespace") queue.namespaceName = std::string(field.get_string().value);
            else if (key == "owner") queue.owner = std::string(field.get_string().value);
            else if (key == "name") queue.name = std::string(field.get_string().value);
            else if (key == "ern") queue.ern = std::string(field.get_string().value);
            else if (key == "size") queue.size = getBsonInt(field);
            else if (key == "delay") queue.delay = getBsonInt(field);
            else if (key == "available") queue.available = getBsonInt(field);
            else if (key == "delayed") queue.delayed = getBsonInt(field);
            else if (key == "invisible") queue.invisible = getBsonInt(field);
            else if (key == "visibility") queue.visibility = getBsonInt(field);
            else if (key == "maxMessageLength") queue.maxMessageLength = getBsonInt(field);
            else if (key == "maxReceiveCount") queue.maxReceiveCount = getBsonInt(field);
            else if (key == "deadLetterQueueErn") queue.deadLetterQueueErn = std::string(field.get_string().value);
            else if (key == "created") queue.created = system_clock::time_point{field.get_date().value};
            else if (key == "modified") queue.modified = system_clock::time_point{field.get_date().value};
            else if (key == "tags") {
                for (const auto &tag: field.get_document().view()) {
                    queue.tags[std::string(tag.key())] = std::string(tag.get_string().value);
                }
            }
        }
        return queue;
    }

}// namespace Euclid::Database::Entity::SQS