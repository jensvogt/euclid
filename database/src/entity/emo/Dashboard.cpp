// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 10/20/26.
//

#include <euclid/database/entity/emo/Dashboard.h>

namespace Euclid::Database::Entity::Monitoring {

    namespace {
        bsoncxx::types::b_date toDate(const std::chrono::system_clock::time_point &timestamp) {
            return bsoncxx::types::b_date{std::chrono::duration_cast<std::chrono::milliseconds>(timestamp.time_since_epoch())};
        }
    }// namespace

    bsoncxx::document::value Dashboard::toDocument() const {

        return bsoncxx::builder::basic::make_document(
                bsoncxx::builder::basic::kvp("userId", userId),
                bsoncxx::builder::basic::kvp("name", name),
                // Text, not a subdocument - see the note on Dashboard::document.
                bsoncxx::builder::basic::kvp("document", document),
                bsoncxx::builder::basic::kvp("created", toDate(created)),
                bsoncxx::builder::basic::kvp("modified", toDate(modified)));
    }

    Dashboard Dashboard::fromDocument(const std::optional<bsoncxx::document::view> &doc) {
        if (!doc) return {};

        Dashboard dashboard;
        for (const auto &field: *doc) {
            if (const auto key = field.key(); key == "userId") dashboard.userId = std::string(field.get_string().value);
            else if (key == "name") dashboard.name = std::string(field.get_string().value);
            else if (key == "document") dashboard.document = std::string(field.get_string().value);
            else if (key == "created") dashboard.created = std::chrono::system_clock::time_point{field.get_date().value};
            else if (key == "modified") dashboard.modified = std::chrono::system_clock::time_point{field.get_date().value};
            else if (key == "_id") dashboard.oid = field.get_oid().value.to_string();
        }
        return dashboard;
    }

}// namespace Euclid::Database::Entity::Monitoring
