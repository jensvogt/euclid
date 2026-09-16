// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <algorithm>
#include <array>
#include <string_view>

// Boost includes
#include <boost/json.hpp>

// Euclid includes
#include <euclid/database/entity/ead/AuditEvent.h>

namespace Euclid::Database::Entity::EAD {

    namespace {

        using bsoncxx::builder::basic::kvp;
        using bsoncxx::builder::basic::make_document;

        constexpr auto kRedacted = "***";

        // Substrings, not exact names, and matched without regard to case: euclid spells the same
        // secret as password, newPassword, oldPassword and passwordHash across its DTOs, and a
        // rule that had to name each one would miss the fourth.
        constexpr std::array kSensitive{
                std::string_view{"password"},
                std::string_view{"secret"},
                std::string_view{"token"},
                std::string_view{"credential"},
                std::string_view{"privatekey"},
                std::string_view{"accesskey"},
                std::string_view{"signature"},
                std::string_view{"passphrase"},
                // The bytes of an object or a message. Not a secret as such, but a body can be
                // megabytes and an audit row is not where it belongs - see also the non-JSON case.
                std::string_view{"body"},
        };

        bool isSensitive(const std::string_view name) {
            std::string lowered(name);
            std::ranges::transform(lowered, lowered.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return std::ranges::any_of(kSensitive, [&lowered](const auto &needle) {
                return lowered.find(needle) != std::string::npos;
            });
        }

        // Walks the whole body rather than its top level: a secret nested inside an object or an
        // array of them is still a secret, and euclid's DTOs nest freely.
        void redactInto(boost::json::value &value) {

            if (value.is_object()) {
                for (auto &[key, nested]: value.as_object()) {
                    if (isSensitive(key)) {
                        nested = kRedacted;
                        continue;
                    }
                    redactInto(nested);
                }
                return;
            }

            if (value.is_array()) {
                for (auto &element: value.as_array()) redactInto(element);
            }
        }

    }// namespace

    std::string Redact(const std::string &body) {

        if (body.empty()) return {};

        boost::json::value parsed;
        try {
            parsed = boost::json::parse(body);
        } catch (const std::exception &) {
            // Not JSON, so there is no field structure to redact by and no way to tell a secret
            // from anything else. Recorded as its size: an upload's bytes are not an audit entry,
            // and guessing at them is how one ends up storing a certificate.
            return "<" + std::to_string(body.size()) + " bytes, not JSON>";
        }

        redactInto(parsed);
        return boost::json::serialize(parsed);
    }

    bsoncxx::document::value AuditEvent::toDocument() const {

        bsoncxx::builder::basic::document document;
        document.append(
                kvp("accountId", accountId),
                kvp("namespace", nameSpace),
                kvp("userId", userId),
                kvp("moduleName", moduleName),
                kvp("command", command),
                kvp("parameters", parameters),
                kvp("status", static_cast<int64_t>(status)),
                kvp("created", bsoncxx::types::b_date(created)));

        // Only when retention is on. A TTL index ignores a document without the field, so an
        // installation that turns retention off keeps what it already has rather than having it
        // swept the moment somebody turns it back on.
        if (expiresAt.time_since_epoch().count() > 0) {
            document.append(kvp("expiresAt", bsoncxx::types::b_date(expiresAt)));
        }

        return document.extract();
    }

    AuditEvent AuditEvent::fromDocument(const std::optional<bsoncxx::document::view> &document) {

        AuditEvent event;
        if (!document.has_value()) return event;

        for (const auto &field: document.value()) {
            const auto key = std::string(field.key());

            if (key == "_id" && field.type() == bsoncxx::type::k_oid) event.oid = field.get_oid().value.to_string();
            else if (key == "accountId" && field.type() == bsoncxx::type::k_string) event.accountId = std::string(field.get_string().value);
            else if (key == "namespace" && field.type() == bsoncxx::type::k_string) event.nameSpace = std::string(field.get_string().value);
            else if (key == "userId" && field.type() == bsoncxx::type::k_string) event.userId = std::string(field.get_string().value);
            else if (key == "moduleName" && field.type() == bsoncxx::type::k_string) event.moduleName = std::string(field.get_string().value);
            else if (key == "command" && field.type() == bsoncxx::type::k_string) event.command = std::string(field.get_string().value);
            else if (key == "parameters" && field.type() == bsoncxx::type::k_string) event.parameters = std::string(field.get_string().value);
            else if (key == "status" && field.type() == bsoncxx::type::k_int64) event.status = field.get_int64().value;
            else if (key == "status" && field.type() == bsoncxx::type::k_int32) event.status = field.get_int32().value;
            else if (key == "created" && field.type() == bsoncxx::type::k_date) event.created = std::chrono::system_clock::time_point{field.get_date().value};
            else if (key == "expiresAt" && field.type() == bsoncxx::type::k_date) event.expiresAt = std::chrono::system_clock::time_point{field.get_date().value};
        }
        return event;
    }

}// namespace Euclid::Database::Entity::EAD
