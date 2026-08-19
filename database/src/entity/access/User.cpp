//
// Created by vogje01 on 8/16/26.
//

#include <bsoncxx/builder/basic/array.hpp>
#include <euclid/database/entity/access/User.h>

namespace Euclid::Database::Entity::Access {

    namespace {

        bsoncxx::document::value accessKeyToDocument(const AccessKey &key) {
            return bsoncxx::builder::basic::make_document(
                    bsoncxx::builder::basic::kvp("accessKeyId", key.accessKeyId),
                    bsoncxx::builder::basic::kvp("secretAccessKey", key.secretAccessKey),
                    bsoncxx::builder::basic::kvp("active", key.active),
                    bsoncxx::builder::basic::kvp("createdAt", key.createdAt));
        }

        AccessKey accessKeyFromDocument(const bsoncxx::document::view &document) {
            AccessKey key;
            for (const auto &field: document) {
                if (const auto k = field.key(); k == "accessKeyId") key.accessKeyId = std::string(field.get_string().value);
                else if (k == "secretAccessKey") key.secretAccessKey = std::string(field.get_string().value);
                else if (k == "active") key.active = field.get_bool().value;
                else if (k == "createdAt") key.createdAt = std::string(field.get_string().value);
            }
            return key;
        }

        bsoncxx::document::value sessionToDocument(const Session &session) {
            return bsoncxx::builder::basic::make_document(
                    bsoncxx::builder::basic::kvp("createdAt", session.createdAt),
                    bsoncxx::builder::basic::kvp("expiresAt", session.expiresAt));
        }

        Session sessionFromDocument(const bsoncxx::document::view &document) {
            Session session;
            for (const auto &field: document) {
                if (const auto k = field.key(); k == "createdAt") session.createdAt = std::string(field.get_string().value);
                else if (k == "expiresAt") session.expiresAt = std::string(field.get_string().value);
            }
            return session;
        }

    }// namespace

    bsoncxx::document::value User::toDocument() const {

        bsoncxx::builder::basic::array accessKeysArray;
        for (const auto &key: accessKeys) accessKeysArray.append(accessKeyToDocument(key));

        bsoncxx::builder::basic::array sessionsArray;
        for (const auto &session: sessions) sessionsArray.append(sessionToDocument(session));

        return bsoncxx::builder::basic::make_document(
                bsoncxx::builder::basic::kvp("userId", userId),
                bsoncxx::builder::basic::kvp("password", password),
                bsoncxx::builder::basic::kvp("email", email),
                bsoncxx::builder::basic::kvp("accountId", accountId),
                bsoncxx::builder::basic::kvp("region", region),
                bsoncxx::builder::basic::kvp("isAdmin", isAdmin),
                bsoncxx::builder::basic::kvp("accessKeys", accessKeysArray),
                bsoncxx::builder::basic::kvp("sessions", sessionsArray));
    }

    User User::fromDocument(const std::optional<bsoncxx::document::view> &document) {
        if (!document) return {};

        User user;
        for (const auto &field: *document) {
            if (const auto key = field.key(); key == "_id") user.oid = field.get_oid().value.to_string();
            else if (key == "userId") user.userId = std::string(field.get_string().value);
            else if (key == "password") user.password = std::string(field.get_string().value);
            else if (key == "email") user.email = std::string(field.get_string().value);
            else if (key == "accountId") user.accountId = std::string(field.get_string().value);
            else if (key == "region") user.region = std::string(field.get_string().value);
            else if (key == "isAdmin") user.isAdmin = field.get_bool().value;
            else if (key == "accessKeys") {
                for (const auto &elem: field.get_array().value) user.accessKeys.push_back(accessKeyFromDocument(elem.get_document().value));
            } else if (key == "sessions") {
                for (const auto &elem: field.get_array().value) user.sessions.push_back(sessionFromDocument(elem.get_document().value));
            }
        }
        return user;
    }

}// namespace Euclid::Database::Entity::Access
