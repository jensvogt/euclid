// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <chrono>
#include <map>
#include <optional>
#include <string>

// MongoDB includes
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view-fwd.hpp>

namespace Euclid::Database::Entity::EDS {

    /**
     * @brief A zone EDS answers for: a domain name, and every record below it that was stored.
     *
     * @par
     * The name is unique across the whole installation rather than within an account, unlike
     * every other named resource. DNS has one namespace: a query for "example.com" carries no
     * account, so two accounts each holding a zone of that name would leave EDS no way to choose.
     * The account and namespace say whose it is - who may change it - not where it is visible.
     *
     * @author jensvogt47\@gmail.com
     */
    struct HostedZone {

        /**
         * @brief ID
         */
        std::string oid;

        /**
         * @brief The apex, lowercased and without a trailing dot: "example.com".
         */
        std::string name;

        /**
         * @brief Euclid resource name.
         */
        std::string ern;

        /**
         * @brief Account the zone belongs to.
         */
        std::string accountId;

        /**
         * @brief Region the zone was created in.
         */
        std::string region;

        /**
         * @brief Namespace the zone belongs to, or empty for an unscoped one.
         */
        std::string nameSpace;

        /**
         * @brief TTL, in seconds, of a record that names none - and how long a resolver may cache
         * an answer saying a name or type is not there.
         */
        long ttl{300};

        /**
         * @brief Free text, for whoever lists the zones.
         */
        std::string description;

        /**
         * @brief Free-form tags, as tables, queues and buckets have.
         */
        std::map<std::string, std::string> tags;

        /**
         * @brief Creation timestamp.
         */
        std::chrono::system_clock::time_point created;

        /**
         * @brief When the zone or any record in it last changed. The SOA serial is made from
         * this, so a secondary or a cache that compares serials sees every change.
         */
        std::chrono::system_clock::time_point modified;

        [[nodiscard]]
        bsoncxx::document::value toDocument() const;

        static HostedZone fromDocument(const std::optional<bsoncxx::document::view> &document);
    };

}// namespace Euclid::Database::Entity::EDS
