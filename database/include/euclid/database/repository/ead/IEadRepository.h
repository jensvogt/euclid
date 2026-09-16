// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <string>
#include <vector>

// Euclid includes
#include <euclid/database/entity/ead/AuditEvent.h>

namespace Euclid::Database {

    /**
     * @brief The audit trail: what was run, by whom, in which account.
     *
     * @par Written by every module, served by one
     * The write side is not EAD's. Every module records its own commands as it answers them - see
     * Core::HttpActionServer::Dispatch() - because an audit that depended on another module being
     * up would be missing exactly the entries somebody restarted things to hide. EAD reads what
     * they wrote.
     *
     * @author jensvogt47\@gmail.com
     */
    class IEadRepository {

      public:

        virtual ~IEadRepository() = default;

        /**
         * @brief Records one command.
         *
         * @param event what was run, by whom.
         * @return the stored event.
         */
        virtual Entity::EAD::AuditEvent createEvent(const Entity::EAD::AuditEvent &event) = 0;

        /**
         * @brief One page of the trail, newest first.
         *
         * @par
         * Newest first because that is the question people ask of an audit - "what just happened"
         * - and a trail that paged oldest first would put the answer on the last page.
         *
         * @param accountId only this account, or empty for every one the caller may see.
         * @param userId only this user, empty for all.
         * @param moduleName only this module, empty for all.
         * @param command only this command, empty for all.
         * @param pageSize how many.
         * @param pageIndex which page.
         * @return the events.
         */
        virtual std::vector<Entity::EAD::AuditEvent> listEvents(const std::string &accountId, const std::string &userId,
                                                                const std::string &moduleName, const std::string &command,
                                                                long pageSize, long pageIndex) const = 0;

        /**
         * @brief How many the same filter matches.
         */
        virtual long countEvents(const std::string &accountId, const std::string &userId,
                                 const std::string &moduleName, const std::string &command) const = 0;

        /**
         * @brief Removes everything older than a moment, and says how much went.
         *
         * @par
         * Retention is normally the database's job through the TTL index, so this is for the
         * installation that changed its mind - shortened a retention that had already let years
         * accumulate, or needs a specific window gone.
         *
         * @param before events created before this are removed.
         * @return how many were removed.
         */
        virtual long purgeEvents(const std::chrono::system_clock::time_point &before) = 0;
    };

}// namespace Euclid::Database
