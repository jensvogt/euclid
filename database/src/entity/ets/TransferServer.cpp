//
// Created by vogje01 on 8/31/26.
//

#include <bsoncxx/builder/basic/array.hpp>
#include <euclid/database/entity/ets/TransferServer.h>

namespace Euclid::Database::Entity::ETS {

    namespace {
        // Ports and passive-port bounds can sit in the collection as either BSON int32 or int64:
        // toDocument() writes int64, but documents written before that cast existed - and any
        // inserted by hand, a migration or mongosh - carry int32, which get_int64() rejects with a
        // type mismatch rather than widening. The exception escaped fromDocument() into the
        // repository's catch-all, dropping every server from the listing at once while
        // countServers(), which never parses an entity, kept reporting them - see
        // MongoEtsRepository::listServers.
        long getBsonInt(const bsoncxx::document::element &field) {
            if (field.type() == bsoncxx::type::k_int64) return field.get_int64().value;
            if (field.type() == bsoncxx::type::k_int32) return field.get_int32().value;
            return 0;
        }
    }// namespace

    bsoncxx::document::value TransferServer::toDocument() const {

        bsoncxx::builder::basic::array userIdsArray;
        for (const auto &userId: userIds) userIdsArray.append(userId);

        bsoncxx::builder::basic::array userGroupsArray;
        for (const auto &group: userGroups) userGroupsArray.append(group);

        bsoncxx::builder::basic::array directoriesArray;
        for (const auto &directory: directories) directoriesArray.append(directory);

        return bsoncxx::builder::basic::make_document(
                bsoncxx::builder::basic::kvp("serverId", serverId),
                bsoncxx::builder::basic::kvp("ern", ern),
                bsoncxx::builder::basic::kvp("accountId", accountId),
                bsoncxx::builder::basic::kvp("region", region),
                bsoncxx::builder::basic::kvp("protocol", TransferProtocolToString(protocol)),
                bsoncxx::builder::basic::kvp("address", address),
                bsoncxx::builder::basic::kvp("port", static_cast<std::int64_t>(port)),
                bsoncxx::builder::basic::kvp("bucketErn", bucketErn),
                bsoncxx::builder::basic::kvp("bucketName", bucketName),
                bsoncxx::builder::basic::kvp("homeDirectory", homeDirectory),
                bsoncxx::builder::basic::kvp("userIds", userIdsArray),
                bsoncxx::builder::basic::kvp("userGroups", userGroupsArray),
                bsoncxx::builder::basic::kvp("directories", directoriesArray),
                bsoncxx::builder::basic::kvp("desiredState", TransferServerStateToString(desiredState)),
                bsoncxx::builder::basic::kvp("hostKey", hostKey),
                bsoncxx::builder::basic::kvp("pasvMin", static_cast<std::int64_t>(pasvMin)),
                bsoncxx::builder::basic::kvp("pasvMax", static_cast<std::int64_t>(pasvMax)),
                bsoncxx::builder::basic::kvp("created", bsoncxx::types::b_date(created)),
                bsoncxx::builder::basic::kvp("modified", bsoncxx::types::b_date(modified)));
    }

    TransferServer TransferServer::fromDocument(const std::optional<bsoncxx::document::view> &document) {
        if (!document) return {};

        TransferServer server;
        for (const auto &field: *document) {
            if (const auto key = field.key(); key == "_id") server.oid = field.get_oid().value.to_string();
            else if (key == "serverId") server.serverId = std::string(field.get_string().value);
            else if (key == "ern") server.ern = std::string(field.get_string().value);
            else if (key == "accountId") server.accountId = std::string(field.get_string().value);
            else if (key == "region") server.region = std::string(field.get_string().value);
            else if (key == "protocol") server.protocol = TransferProtocolFromString(std::string(field.get_string().value));
            else if (key == "address") server.address = std::string(field.get_string().value);
            else if (key == "port") server.port = getBsonInt(field);
            else if (key == "bucketErn") server.bucketErn = std::string(field.get_string().value);
            else if (key == "bucketName") server.bucketName = std::string(field.get_string().value);
            else if (key == "homeDirectory") server.homeDirectory = std::string(field.get_string().value);
            else if (key == "userIds") {
                for (const auto &elem: field.get_array().value) server.userIds.emplace_back(elem.get_string().value);
            } else if (key == "userGroups") {
                for (const auto &elem: field.get_array().value) server.userGroups.emplace_back(elem.get_string().value);
            } else if (key == "directories") {
                for (const auto &elem: field.get_array().value) server.directories.emplace_back(elem.get_string().value);
            } else if (key == "desiredState") server.desiredState = TransferServerStateFromString(std::string(field.get_string().value));
            else if (key == "hostKey") server.hostKey = std::string(field.get_string().value);
            else if (key == "pasvMin") server.pasvMin = getBsonInt(field);
            else if (key == "pasvMax") server.pasvMax = getBsonInt(field);
            else if (key == "created") server.created = std::chrono::system_clock::time_point{field.get_date().value};
            else if (key == "modified") server.modified = std::chrono::system_clock::time_point{field.get_date().value};
        }
        return server;
    }

}// namespace Euclid::Database::Entity::ETS
