// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/7/26.
//

#include <bsoncxx/builder/basic/array.hpp>

#include <euclid/database/entity/ekm/Certificate.h>

namespace Euclid::Database::Entity::EKM {

    bsoncxx::document::value Certificate::toDocument() const {

        bsoncxx::builder::basic::document tagsDoc;
        for (const auto &[k, v]: tags) {
            tagsDoc.append(bsoncxx::builder::basic::kvp(k, v));
        }

        bsoncxx::builder::basic::array altNameArray;
        for (const auto &name: subjectAltNames) altNameArray.append(name);

        return bsoncxx::builder::basic::make_document(
                bsoncxx::builder::basic::kvp("region", region),
                bsoncxx::builder::basic::kvp("accountId", accountId),
                bsoncxx::builder::basic::kvp("namespace", nameSpace),
                bsoncxx::builder::basic::kvp("ern", ern),
                bsoncxx::builder::basic::kvp("name", name),
                bsoncxx::builder::basic::kvp("description", description),
                bsoncxx::builder::basic::kvp("certificate", certificatePem),
                bsoncxx::builder::basic::kvp("privateKey", privateKeyPem),
                bsoncxx::builder::basic::kvp("subject", subject),
                bsoncxx::builder::basic::kvp("issuer", issuer),
                bsoncxx::builder::basic::kvp("serialNumber", serialNumber),
                bsoncxx::builder::basic::kvp("fingerprint", fingerprint),
                bsoncxx::builder::basic::kvp("subjectAltNames", altNameArray),
                bsoncxx::builder::basic::kvp("generated", generated),
                bsoncxx::builder::basic::kvp("notBefore", bsoncxx::types::b_date{std::chrono::duration_cast<std::chrono::milliseconds>(notBefore.time_since_epoch())}),
                bsoncxx::builder::basic::kvp("notAfter", bsoncxx::types::b_date{std::chrono::duration_cast<std::chrono::milliseconds>(notAfter.time_since_epoch())}),
                bsoncxx::builder::basic::kvp("tags", tagsDoc.extract()));
    }

    Certificate Certificate::fromDocument(const std::optional<bsoncxx::document::view> &document) {
        if (!document) return {};

        Certificate certificate;
        for (const auto &field: *document) {
            if (const auto key = field.key(); key == "_id") certificate.oid = field.get_oid().value.to_string();
            else if (key == "region") certificate.region = std::string(field.get_string().value);
            else if (key == "accountId") certificate.accountId = std::string(field.get_string().value);
            else if (key == "namespace") certificate.nameSpace = std::string(field.get_string().value);
            else if (key == "ern") certificate.ern = std::string(field.get_string().value);
            else if (key == "name") certificate.name = std::string(field.get_string().value);
            else if (key == "description") certificate.description = std::string(field.get_string().value);
            else if (key == "certificate") certificate.certificatePem = std::string(field.get_string().value);
            else if (key == "privateKey") certificate.privateKeyPem = std::string(field.get_string().value);
            else if (key == "subject") certificate.subject = std::string(field.get_string().value);
            else if (key == "issuer") certificate.issuer = std::string(field.get_string().value);
            else if (key == "serialNumber") certificate.serialNumber = std::string(field.get_string().value);
            else if (key == "fingerprint") certificate.fingerprint = std::string(field.get_string().value);
            else if (key == "generated") certificate.generated = field.get_bool().value;
            else if (key == "notBefore") certificate.notBefore = system_clock::time_point{field.get_date().value};
            else if (key == "notAfter") certificate.notAfter = system_clock::time_point{field.get_date().value};
            else if (key == "created") certificate.created = system_clock::time_point{field.get_date().value};
            else if (key == "modified") certificate.modified = system_clock::time_point{field.get_date().value};
            else if (key == "subjectAltNames") {
                for (const auto &name: field.get_array().value) {
                    certificate.subjectAltNames.emplace_back(name.get_string().value);
                }
            } else if (key == "tags") {
                for (const auto &tag: field.get_document().view()) {
                    certificate.tags[std::string(tag.key())] = std::string(tag.get_string().value);
                }
            }
        }
        return certificate;
    }

}// namespace Euclid::Database::Entity::EKM
