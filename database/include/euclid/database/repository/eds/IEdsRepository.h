// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <chrono>
#include <optional>
#include <string>
#include <vector>

// Euclid includes
#include <euclid/database/entity/eds/HostedZone.h>
#include <euclid/database/entity/eds/Record.h>

namespace Euclid::Database {

    /**
     * @brief Interface for the domain name service's zones and records.
     *
     * @par
     * A zone is found by its name alone - see Entity::EDS::HostedZone for why its name is unique
     * across the installation. Listing is still per account and namespace, since that is what a
     * caller is allowed to see.
     *
     * @author jensvogt47\@gmail.com
     */
    class IEdsRepository {

    public:
        virtual ~IEdsRepository() = default;

        // ── Zones ────────────────────────────────────────────────────────────

        /**
         * @brief Stores a new zone.
         *
         * @throws std::exception when the name is taken - the unique index decides, so two
         * concurrent creates cannot both succeed.
         */
        virtual Entity::EDS::HostedZone createZone(Entity::EDS::HostedZone &zone) = 0;

        /**
         * @brief A zone by its name, whoever owns it.
         */
        [[nodiscard]]
        virtual std::optional<Entity::EDS::HostedZone> findZone(const std::string &name) const = 0;

        /**
         * @brief One account's zones in one namespace, by name.
         *
         * @param prefix only zones whose name starts with this; empty matches all.
         * @param pageSize most to return; 0 or less means no limit.
         * @param pageIndex zero-based page, applied when pageSize is set.
         */
        [[nodiscard]]
        virtual std::vector<Entity::EDS::HostedZone> listZones(const std::string &accountId, const std::string &nameSpace,
                                                               const std::string &prefix, long pageSize, long pageIndex) const = 0;

        /**
         * @brief The count that goes with listZones().
         */
        [[nodiscard]]
        virtual long countZones(const std::string &accountId, const std::string &nameSpace, const std::string &prefix) const = 0;

        /**
         * @brief Every zone of every account - what the name server loads.
         */
        [[nodiscard]]
        virtual std::vector<Entity::EDS::HostedZone> listAllZones() const = 0;

        /**
         * @brief Deletes a zone and its records.
         *
         * @return how many record sets went with it.
         */
        virtual long deleteZone(const std::string &name) = 0;

        /**
         * @brief Writes a zone's TTL and description, and stamps it as changed.
         *
         * @par
         * Those two and modified only: the name is what records and grants refer to, and the
         * owner is who may change it, so neither is something an update can move.
         *
         * @return false when there is no such zone.
         */
        virtual bool updateZone(const Entity::EDS::HostedZone &zone) = 0;

        /**
         * @brief Stamps a zone as changed, which moves its SOA serial.
         */
        virtual void touchZone(const std::string &name, std::chrono::system_clock::time_point when) = 0;

        // ── Records ──────────────────────────────────────────────────────────

        /**
         * @brief Writes a record set, replacing the one of the same name and type if there is one.
         *
         * @return the stored set.
         */
        virtual Entity::EDS::Record putRecord(Entity::EDS::Record &record) = 0;

        /**
         * @brief Every record set at one name in a zone, whatever its type.
         */
        [[nodiscard]]
        virtual std::vector<Entity::EDS::Record> findRecords(const std::string &zoneName, const std::string &name) const = 0;

        /**
         * @brief Removes one record set.
         *
         * @return true if there was one.
         */
        virtual bool deleteRecord(const std::string &zoneName, const std::string &name, const std::string &type) = 0;

        /**
         * @brief A zone's record sets, by name then type.
         *
         * @param name only sets at this owner name; empty means all.
         * @param type only sets of this type; empty means all.
         */
        [[nodiscard]]
        virtual std::vector<Entity::EDS::Record> listRecords(const std::string &zoneName, const std::string &name,
                                                             const std::string &type, long pageSize, long pageIndex) const = 0;

        /**
         * @brief The count that goes with listRecords().
         */
        [[nodiscard]]
        virtual long countRecords(const std::string &zoneName, const std::string &name, const std::string &type) const = 0;

        /**
         * @brief Every record set of every zone - what the name server loads.
         */
        [[nodiscard]]
        virtual std::vector<Entity::EDS::Record> listAllRecords() const = 0;
    };

}// namespace Euclid::Database
