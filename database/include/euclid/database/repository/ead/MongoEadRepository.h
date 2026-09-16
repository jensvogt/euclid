// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// MongoDB includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>

// Euclid includes
#include <euclid/core/LogStream.h>
#include <euclid/database/Database.h>
#include <euclid/database/repository/ead/IEadRepository.h>

namespace Euclid::Database {

    using namespace bsoncxx::builder::basic;

    /**
     * @brief The audit trail, in the document store.
     *
     * @author jensvogt47\@gmail.com
     */
    class MongoEadRepository final : public IEadRepository {

      public:

        /**
         * @brief Constructor, ensuring the collection's indexes exist.
         */
        MongoEadRepository();

        Entity::EAD::AuditEvent createEvent(const Entity::EAD::AuditEvent &event) override;

        [[nodiscard]]
        std::vector<Entity::EAD::AuditEvent> listEvents(const std::string &accountId, const std::string &userId,
                                                        const std::string &moduleName, const std::string &command,
                                                        long pageSize, long pageIndex) const override;

        [[nodiscard]]
        long countEvents(const std::string &accountId, const std::string &userId,
                         const std::string &moduleName, const std::string &command) const override;

        long purgeEvents(const std::chrono::system_clock::time_point &before) override;

      private:

        /**
         * @brief Collection holding the trail.
         */
        static constexpr auto COLLECTION = "ead_audit";
    };

}// namespace Euclid::Database
