//
// Created by vogje01 on 9/1/26.
//

#include <bsoncxx/builder/basic/array.hpp>
#include <euclid/database/entity/eap/Application.h>

namespace Euclid::Database::Entity::EAP {

    bsoncxx::document::value Application::toDocument() const {

        bsoncxx::builder::basic::array argumentsArray;
        for (const auto &argument: arguments) argumentsArray.append(argument);

        bsoncxx::builder::basic::document environmentDoc;
        for (const auto &[name, value]: environment) environmentDoc.append(bsoncxx::builder::basic::kvp(name, value));

        return bsoncxx::builder::basic::make_document(
                bsoncxx::builder::basic::kvp("applicationId", applicationId),
                bsoncxx::builder::basic::kvp("ern", ern),
                bsoncxx::builder::basic::kvp("accountId", accountId),
                bsoncxx::builder::basic::kvp("region", region),
                bsoncxx::builder::basic::kvp("runtime", RuntimeToString(runtime)),
                bsoncxx::builder::basic::kvp("bucketErn", bucketErn),
                bsoncxx::builder::basic::kvp("artifactKey", artifactKey),
                bsoncxx::builder::basic::kvp("command", command),
                bsoncxx::builder::basic::kvp("arguments", argumentsArray),
                bsoncxx::builder::basic::kvp("environment", environmentDoc.extract()),
                bsoncxx::builder::basic::kvp("userId", userId),
                bsoncxx::builder::basic::kvp("minInstances", static_cast<std::int64_t>(minInstances)),
                bsoncxx::builder::basic::kvp("maxInstances", static_cast<std::int64_t>(maxInstances)),
                bsoncxx::builder::basic::kvp("readyTimeoutMs", static_cast<std::int64_t>(readyTimeoutMs)),
                bsoncxx::builder::basic::kvp("desiredState", ApplicationStateToString(desiredState)),
                bsoncxx::builder::basic::kvp("created", bsoncxx::types::b_date(created)),
                bsoncxx::builder::basic::kvp("modified", bsoncxx::types::b_date(modified)));
    }

    Application Application::fromDocument(const std::optional<bsoncxx::document::view> &document) {
        if (!document) return {};

        Application application;
        for (const auto &field: *document) {
            if (const auto key = field.key(); key == "_id") application.oid = field.get_oid().value.to_string();
            else if (key == "applicationId") application.applicationId = std::string(field.get_string().value);
            else if (key == "ern") application.ern = std::string(field.get_string().value);
            else if (key == "accountId") application.accountId = std::string(field.get_string().value);
            else if (key == "region") application.region = std::string(field.get_string().value);
            else if (key == "runtime") application.runtime = RuntimeFromString(std::string(field.get_string().value));
            else if (key == "bucketErn") application.bucketErn = std::string(field.get_string().value);
            else if (key == "artifactKey") application.artifactKey = std::string(field.get_string().value);
            else if (key == "command") application.command = std::string(field.get_string().value);
            else if (key == "arguments") {
                for (const auto &elem: field.get_array().value) application.arguments.emplace_back(elem.get_string().value);
            } else if (key == "environment") {
                for (const auto &elem: field.get_document().value) application.environment[std::string(elem.key())] = std::string(elem.get_string().value);
            } else if (key == "userId") application.userId = std::string(field.get_string().value);
            else if (key == "minInstances") application.minInstances = static_cast<long>(field.get_int64().value);
            else if (key == "maxInstances") application.maxInstances = static_cast<long>(field.get_int64().value);
            else if (key == "readyTimeoutMs") application.readyTimeoutMs = static_cast<long>(field.get_int64().value);
            else if (key == "desiredState") application.desiredState = ApplicationStateFromString(std::string(field.get_string().value));
            else if (key == "created") application.created = std::chrono::system_clock::time_point{field.get_date().value};
            else if (key == "modified") application.modified = std::chrono::system_clock::time_point{field.get_date().value};
        }
        return application;
    }

}// namespace Euclid::Database::Entity::EAP
