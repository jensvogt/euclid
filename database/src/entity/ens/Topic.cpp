//
// Created by vogje01 on 03/09/2023.
//

#include <euclid/database/entity/ens/Topic.h>

namespace Euclid::Database::Entity::ENS {

    namespace {
        // See ESM's Bucket::getBsonInt for why this tolerates both int32 and int64 - the same
        // failure mode (get_int64() throwing on a document whose counters ended up as int32,
        // silently dropping it from list results) applies to topics.
        long getBsonInt(const bsoncxx::document::element &field) {
            if (field.type() == bsoncxx::type::k_int64) return field.get_int64().value;
            if (field.type() == bsoncxx::type::k_int32) return field.get_int32().value;
            return 0;
        }
    }// namespace

    bsoncxx::document::value Topic::toDocument() const {

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
                bsoncxx::builder::basic::kvp("namespace", nameSpace),
                bsoncxx::builder::basic::kvp("owner", owner),
                bsoncxx::builder::basic::kvp("name", name),
                bsoncxx::builder::basic::kvp("ern", ern),
                bsoncxx::builder::basic::kvp("size", static_cast<int64_t>(size)),
                bsoncxx::builder::basic::kvp("available", static_cast<int64_t>(available)),
                bsoncxx::builder::basic::kvp("send", static_cast<int64_t>(send)),
                bsoncxx::builder::basic::kvp("resend", static_cast<int64_t>(resend)),
                bsoncxx::builder::basic::kvp("maxMessageLength", static_cast<int64_t>(maxMessageLength)),
                bsoncxx::builder::basic::kvp("tags", tagsDoc.extract()));
        // bsoncxx::builder::basic::kvp("defaultMessageAttributes", defaultMessageAttributesDoc.extract()));
    }

    Topic Topic::fromDocument(const std::optional<bsoncxx::document::view> &document) {
        if (!document) return {};

        Topic topic;
        for (const auto &field: *document) {
            if (const auto key = field.key(); key == "_id") topic.oid = field.get_oid().value.to_string();
            else if (key == "region") topic.region = std::string(field.get_string().value);
            else if (key == "accountId") topic.accountId = std::string(field.get_string().value);
            else if (key == "namespace") topic.nameSpace = std::string(field.get_string().value);
            else if (key == "owner") topic.owner = std::string(field.get_string().value);
            else if (key == "name") topic.name = std::string(field.get_string().value);
            else if (key == "ern") topic.ern = std::string(field.get_string().value);
            else if (key == "size") topic.size = getBsonInt(field);
            else if (key == "available") topic.available = getBsonInt(field);
            else if (key == "send") topic.send = getBsonInt(field);
            else if (key == "resend") topic.resend = getBsonInt(field);
            else if (key == "maxMessageLength") topic.maxMessageLength = getBsonInt(field);
            else if (key == "created") topic.created = system_clock::time_point{field.get_date().value};
            else if (key == "modified") topic.modified = system_clock::time_point{field.get_date().value};
            else if (key == "tags") {
                for (const auto &tag: field.get_document().view()) {
                    topic.tags[std::string(tag.key())] = std::string(tag.get_string().value);
                }
            }
        }
        return topic;
    }

}// namespace Euclid::Database::Entity::ENS