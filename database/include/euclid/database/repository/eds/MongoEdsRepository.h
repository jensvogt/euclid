// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// Euclid includes
#include <euclid/core/LogStream.h>
#include <euclid/database/Database.h>
#include <euclid/database/repository/eds/IEdsRepository.h>

namespace Euclid::Database {

    /**
     * @brief EDS repository on Database::collection() - MongoDB, the in-process store or the
     * store EMD holds, as every other repository.
     *
     * @author jensvogt47\@gmail.com
     */
    class MongoEdsRepository final : public IEdsRepository {

    public:
        explicit MongoEdsRepository();

        Entity::EDS::HostedZone createZone(Entity::EDS::HostedZone &zone) override;

        [[nodiscard]]
        std::optional<Entity::EDS::HostedZone> findZone(const std::string &name) const override;

        [[nodiscard]]
        std::vector<Entity::EDS::HostedZone> listZones(const std::string &accountId, const std::string &nameSpace,
                                                       const std::string &prefix, long pageSize, long pageIndex) const override;

        [[nodiscard]]
        long countZones(const std::string &accountId, const std::string &nameSpace, const std::string &prefix) const override;

        [[nodiscard]]
        std::vector<Entity::EDS::HostedZone> listAllZones() const override;

        long deleteZone(const std::string &name) override;

        void touchZone(const std::string &name, std::chrono::system_clock::time_point when) override;

        Entity::EDS::Record putRecord(Entity::EDS::Record &record) override;

        [[nodiscard]]
        std::vector<Entity::EDS::Record> findRecords(const std::string &zoneName, const std::string &name) const override;

        bool deleteRecord(const std::string &zoneName, const std::string &name, const std::string &type) override;

        [[nodiscard]]
        std::vector<Entity::EDS::Record> listRecords(const std::string &zoneName, const std::string &name,
                                                     const std::string &type, long pageSize, long pageIndex) const override;

        [[nodiscard]]
        long countRecords(const std::string &zoneName, const std::string &name, const std::string &type) const override;

        [[nodiscard]]
        std::vector<Entity::EDS::Record> listAllRecords() const override;

    private:
        /**
         * @brief One zone per name, one record set per (zone, name, type).
         */
        void ensureIndexes() const;

        static constexpr auto ZONE_COLLECTION = "eds_zone";
        static constexpr auto RECORD_COLLECTION = "eds_record";
    };

}// namespace Euclid::Database
