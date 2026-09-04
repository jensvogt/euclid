//
// Created by vogje01 on 9/1/26.
//

#include <regex>

#include <bsoncxx/builder/basic/array.hpp>
#include <euclid/database/entity/eap/Application.h>

namespace Euclid::Database::Entity::EAP {

    namespace {
        // Instance counts and the ready timeout can sit in the collection as either BSON int32 or
        // int64: toDocument() writes int64, but documents written before that cast existed - and
        // any inserted by hand, a migration or mongosh - carry int32, which get_int64() rejects
        // with a type mismatch rather than widening. The exception escaped fromDocument() into the
        // repository's catch-all, dropping every application from the listing at once while
        // countApplications(), which never parses an entity, kept reporting them - see
        // MongoEapRepository::listApplications.
        long getBsonInt(const bsoncxx::document::element &field) {
            if (field.type() == bsoncxx::type::k_int64) return field.get_int64().value;
            if (field.type() == bsoncxx::type::k_int32) return field.get_int32().value;
            return 0;
        }
    }// namespace

    bsoncxx::document::value Application::toDocument() const {

        bsoncxx::builder::basic::array argumentsArray;
        for (const auto &argument: arguments) argumentsArray.append(argument);

        bsoncxx::builder::basic::array resourcesArray;
        for (const auto &resource: resources) resourcesArray.append(resource);

        bsoncxx::builder::basic::document environmentDoc;
        for (const auto &[name, value]: environment) environmentDoc.append(bsoncxx::builder::basic::kvp(name, value));

        return bsoncxx::builder::basic::make_document(
                bsoncxx::builder::basic::kvp("applicationId", applicationId),
                bsoncxx::builder::basic::kvp("ern", ern),
                bsoncxx::builder::basic::kvp("accountId", accountId),
                bsoncxx::builder::basic::kvp("region", region),
                bsoncxx::builder::basic::kvp("namespace", nameSpace),
                bsoncxx::builder::basic::kvp("runtime", RuntimeToString(runtime)),
                bsoncxx::builder::basic::kvp("bucketErn", bucketErn),
                bsoncxx::builder::basic::kvp("artifactKey", artifactKey),
                bsoncxx::builder::basic::kvp("version", version),
                bsoncxx::builder::basic::kvp("md5Sum", md5Sum),
                bsoncxx::builder::basic::kvp("command", command),
                bsoncxx::builder::basic::kvp("arguments", argumentsArray),
                bsoncxx::builder::basic::kvp("environment", environmentDoc.extract()),
                bsoncxx::builder::basic::kvp("resources", resourcesArray),
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
            // Absent on every application created before applications carried one.
            else if (key == "namespace") application.nameSpace = std::string(field.get_string().value);
            else if (key == "runtime") application.runtime = RuntimeFromString(std::string(field.get_string().value));
            else if (key == "bucketErn") application.bucketErn = std::string(field.get_string().value);
            else if (key == "artifactKey") application.artifactKey = std::string(field.get_string().value);
            // Absent from every application defined before versions existed, which is why nothing
            // here insists on it: such an application carries an empty version and md5 until its
            // next redeploy, and that redeploy is the one that fills them in.
            else if (key == "version") application.version = std::string(field.get_string().value);
            else if (key == "md5Sum") application.md5Sum = std::string(field.get_string().value);
            else if (key == "command") application.command = std::string(field.get_string().value);
            else if (key == "arguments") {
                for (const auto &elem: field.get_array().value) application.arguments.emplace_back(elem.get_string().value);
            } else if (key == "environment") {
                for (const auto &elem: field.get_document().value) application.environment[std::string(elem.key())] = std::string(elem.get_string().value);
            } else if (key == "resources") {
                for (const auto &elem: field.get_array().value) application.resources.emplace_back(elem.get_string().value);
            } else if (key == "userId") application.userId = std::string(field.get_string().value);
            else if (key == "minInstances") application.minInstances = getBsonInt(field);
            else if (key == "maxInstances") application.maxInstances = getBsonInt(field);
            else if (key == "readyTimeoutMs") application.readyTimeoutMs = getBsonInt(field);
            else if (key == "desiredState") application.desiredState = ApplicationStateFromString(std::string(field.get_string().value));
            else if (key == "created") application.created = std::chrono::system_clock::time_point{field.get_date().value};
            else if (key == "modified") application.modified = std::chrono::system_clock::time_point{field.get_date().value};
        }
        return application;
    }

    std::string VersionFromArtifactName(const std::string &name) {
        static const std::regex pattern(R"((\d+)\.(\d+)\.(\d+))");
        if (std::smatch match; std::regex_search(name, match, pattern)) return match.str();
        return {};
    }

    std::string RedeployRefusal(const std::string &deployedVersion, const std::string &deployedMd5Sum,
                                const std::string &version, const std::string &md5Sum) {

        // The version is deliberately not checked. Redeploying the same version with different
        // bytes is a normal thing to do - a rebuilt snapshot, a fix that keeps the number - and
        // refusing it only meant reaching for update-application to do the same thing without the
        // safeguard below. What the artifact is, rather than what it is called, is what decides
        // whether this is a deployment.
        //
        // Only when there is something to compare against: an application defined before versions
        // existed carries no checksum, and its first redeploy is not going to be refused for it.
        if (!deployedMd5Sum.empty() && !md5Sum.empty() && md5Sum == deployedMd5Sum) {
            return "the artifact is byte for byte the build already deployed"
                   + (deployedVersion.empty() ? std::string() : " as version " + deployedVersion)
                   + " - nothing would change";
        }

        return {};
    }

}// namespace Euclid::Database::Entity::EAP
