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
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view-fwd.hpp>

namespace Euclid::Database::Entity::EDS {

    /**
     * @brief Every record of one type at one name in a zone - a record set.
     *
     * @par
     * Stored as a set rather than one document per value because DNS treats it as one: a set has
     * one TTL (RFC 2181 §5.2), is answered whole, and is what an operator replaces when an address
     * changes. Writing the set is then one upsert, and there is no moment at which half the old
     * addresses and half the new ones are being served.
     *
     * @par
     * Values are kept in presentation form - "10 mail.example.com", not wire bytes - so that what
     * list-records shows is what was put, and so a stored record is readable in the database. They
     * are encoded when the zone is loaded; one that no longer encodes is skipped with a warning
     * rather than taking the zone down with it.
     *
     * @author jensvogt47\@gmail.com
     */
    struct Record {

        /**
         * @brief ID
         */
        std::string oid;

        /**
         * @brief Account of the zone this belongs to, copied so a record can be authorized
         * without reading its zone.
         */
        std::string accountId;

        /**
         * @brief The zone's apex, normalized.
         */
        std::string zoneName;

        /**
         * @brief Owner name, fully qualified and normalized: "www.example.com". The apex itself
         * is a record at the zone's own name; "*.example.com" is a wildcard.
         */
        std::string name;

        /**
         * @brief Type mnemonic, upper case: "A", "MX", "SRV"...
         */
        std::string type;

        /**
         * @brief TTL in seconds; 0 takes the zone's.
         */
        long ttl{};

        /**
         * @brief The values, in presentation form. Never empty: deleting the last value deletes
         * the set.
         */
        std::vector<std::string> values;

        /**
         * @brief Creation timestamp.
         */
        std::chrono::system_clock::time_point created;

        /**
         * @brief Modification timestamp.
         */
        std::chrono::system_clock::time_point modified;

        [[nodiscard]]
        bsoncxx::document::value toDocument() const;

        static Record fromDocument(const std::optional<bsoncxx::document::view> &document);
    };

}// namespace Euclid::Database::Entity::EDS
