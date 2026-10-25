// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <chrono>
#include <optional>
#include <string>
#include <vector>

// MongoDB includes
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>

// Euclid includes
#include <euclid/core/LogStream.h>
#include <euclid/database/Database.h>

namespace Euclid::EAP::Platform {

    /**
     * @brief A platform declaration: the shared objects of a namespace that no application owns.
     *
     * @par
     * An application's folder declares what that application owns, and euclid finds it beside the
     * application's artifact. Some objects belong to no application - the bucket an FTP server
     * writes deliveries into, a topic several services publish to - and those were made by a script
     * somebody kept. A platform declaration is the same folder format for them, applied with
     * `eap apply --platform <name>`, owned by the platform and stored here, since there is no
     * artifact to store it beside.
     *
     * @par
     * It only creates. A platform has no principal: it reaches nothing, so `uses` means nothing for
     * it, and nothing is granted. Applications reach its objects with `uses`, like anybody else's.
     */
    inline constexpr auto kPlatformCollection = "eap_platform";

    /**
     * @brief What the owner tag says for a platform's objects: "platform:<name>", which no
     * application id can be, so a platform object and an application's can never be mistaken.
     */
    inline std::string OwnerTag(const std::string &name) {
        return "platform:" + name;
    }

    struct Record {
        std::string name;
        std::string accountId;
        std::string nameSpace;
        std::string region;
        /// The declaration last applied, as Infrastructure::Write() wrote it.
        std::string declaration;
        /// What it owns, for the next apply to diff against - as an application's record has.
        std::vector<std::string> infrastructure;
        std::chrono::system_clock::time_point modified{};
    };

    namespace detail {

        inline bsoncxx::document::value filter(const std::string &accountId, const std::string &nameSpace, const std::string &name) {
            using bsoncxx::builder::basic::kvp;
            return bsoncxx::builder::basic::make_document(kvp("accountId", accountId), kvp("namespace", nameSpace), kvp("name", name));
        }

        inline Record fromDocument(const bsoncxx::document::view &document) {
            Record record;
            for (const auto &field: document) {
                const auto key = field.key();
                if (key == "name") record.name = std::string(field.get_string().value);
                else if (key == "accountId") record.accountId = std::string(field.get_string().value);
                else if (key == "namespace") record.nameSpace = std::string(field.get_string().value);
                else if (key == "region") record.region = std::string(field.get_string().value);
                else if (key == "declaration") record.declaration = std::string(field.get_string().value);
                else if (key == "modified") record.modified = std::chrono::system_clock::time_point{field.get_date().value};
                else if (key == "infrastructure") {
                    for (const auto &ern: field.get_array().value) record.infrastructure.emplace_back(ern.get_string().value);
                }
            }
            return record;
        }

    }// namespace detail

    inline std::optional<Record> Find(const std::string &accountId, const std::string &nameSpace, const std::string &name) {
        try {
            const auto collection = Database::Database::instance().collection(kPlatformCollection);
            if (const auto found = collection.find_one(detail::filter(accountId, nameSpace, name).view())) return detail::fromDocument(found->view());
        } catch (const std::exception &e) {
            log_error << "Find platform failed, name: " << name << ", error: " << e.what();
        }
        return std::nullopt;
    }

    inline std::vector<Record> List(const std::string &accountId, const std::string &nameSpace) {
        std::vector<Record> records;
        try {
            using bsoncxx::builder::basic::kvp;
            const auto collection = Database::Database::instance().collection(kPlatformCollection);
            for (const auto &document: collection.find(bsoncxx::builder::basic::make_document(kvp("accountId", accountId), kvp("namespace", nameSpace)))) {
                records.push_back(detail::fromDocument(document));
            }
        } catch (const std::exception &e) {
            log_error << "List platforms failed, error: " << e.what();
        }
        return records;
    }

    inline bool Store(const Record &record) {
        using bsoncxx::builder::basic::kvp;
        try {
            bsoncxx::builder::basic::array infrastructure;
            for (const auto &ern: record.infrastructure) infrastructure.append(ern);
            const auto document = bsoncxx::builder::basic::make_document(
                    kvp("name", record.name), kvp("accountId", record.accountId), kvp("namespace", record.nameSpace),
                    kvp("region", record.region), kvp("declaration", record.declaration), kvp("infrastructure", infrastructure),
                    kvp("modified", bsoncxx::types::b_date{std::chrono::system_clock::now()}));

            // Found and replaced, or inserted: one record per name, and the same calls on both the
            // MongoDB and the in-memory store.
            const auto collection = Database::Database::instance().collection(kPlatformCollection);
            const auto filter = detail::filter(record.accountId, record.nameSpace, record.name);
            if (collection.find_one(filter.view()).has_value()) return collection.replace_one(filter.view(), document.view()).has_value();
            return collection.insert_one(document.view()).has_value();

        } catch (const std::exception &e) {
            log_error << "Store platform failed, name: " << record.name << ", error: " << e.what();
            return false;
        }
    }

}// namespace Euclid::EAP::Platform
