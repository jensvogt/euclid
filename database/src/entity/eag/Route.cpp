//
// Created by vogje01 on 9/5/26.
//

#include <bsoncxx/builder/basic/array.hpp>

#include <euclid/database/entity/eag/Route.h>

namespace Euclid::Database::Entity::EAG {

    bsoncxx::document::value Route::toDocument() const {

        bsoncxx::builder::basic::array methodArray;
        for (const auto &method: methods) methodArray.append(method);

        return bsoncxx::builder::basic::make_document(
                bsoncxx::builder::basic::kvp("routeId", routeId),
                bsoncxx::builder::basic::kvp("ern", ern),
                bsoncxx::builder::basic::kvp("accountId", accountId),
                bsoncxx::builder::basic::kvp("region", region),
                bsoncxx::builder::basic::kvp("namespace", nameSpace),
                bsoncxx::builder::basic::kvp("path", path),
                bsoncxx::builder::basic::kvp("applicationId", applicationId),
                bsoncxx::builder::basic::kvp("moduleTarget", moduleTarget),
                bsoncxx::builder::basic::kvp("moduleAction", moduleAction),
                bsoncxx::builder::basic::kvp("methods", methodArray),
                bsoncxx::builder::basic::kvp("authentication", RouteAuthenticationToString(authentication)),
                bsoncxx::builder::basic::kvp("active", active),
                bsoncxx::builder::basic::kvp("created", bsoncxx::types::b_date{std::chrono::duration_cast<std::chrono::milliseconds>(created.time_since_epoch())}),
                bsoncxx::builder::basic::kvp("modified", bsoncxx::types::b_date{std::chrono::duration_cast<std::chrono::milliseconds>(modified.time_since_epoch())}));
    }

    Route Route::fromDocument(const std::optional<bsoncxx::document::view> &doc) {
        if (!doc) return {};

        Route route;
        for (const auto &field: *doc) {
            if (const auto key = field.key(); key == "routeId") route.routeId = std::string(field.get_string().value);
            else if (key == "ern") route.ern = std::string(field.get_string().value);
            else if (key == "accountId") route.accountId = std::string(field.get_string().value);
            else if (key == "region") route.region = std::string(field.get_string().value);
            else if (key == "namespace") route.nameSpace = std::string(field.get_string().value);
            else if (key == "path") route.path = std::string(field.get_string().value);
            else if (key == "applicationId") route.applicationId = std::string(field.get_string().value);
            else if (key == "moduleTarget") route.moduleTarget = std::string(field.get_string().value);
            else if (key == "moduleAction") route.moduleAction = std::string(field.get_string().value);
            else if (key == "methods") {
                for (const auto &method: field.get_array().value) {
                    route.methods.emplace_back(method.get_string().value);
                }
            }
            else if (key == "authentication") {
                // A stored value nobody can parse falls back to NONE - see
                // RouteAuthenticationFromString. Refusing to load the route instead would take a
                // working path offline over a byte in the database.
                route.authentication = RouteAuthenticationFromString(std::string(field.get_string().value)).value_or(RouteAuthentication::NONE);
            }
            else if (key == "active") route.active = field.get_bool().value;
            else if (key == "created") route.created = std::chrono::system_clock::time_point{field.get_date().value};
            else if (key == "modified") route.modified = std::chrono::system_clock::time_point{field.get_date().value};
            else if (key == "_id") route.oid = field.get_oid().value.to_string();
        }
        return route;
    }

}// namespace Euclid::Database::Entity::EAG
