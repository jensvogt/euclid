// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

// Euclid includes
#include <euclid/database/entity/ead/AuditEvent.h>

namespace Euclid::Database {

    /**
     * @brief Writes the audit trail off the request thread.
     *
     * @par Why it is not written inline
     * The record is made in Core::HttpActionServer::Dispatch(), which is the path of every request
     * in the installation. A database insert there would put a round trip on the critical path of
     * every command euclid answers, to write something nobody is waiting for.
     *
     * @par Why the queue is bounded
     * Because the alternative is worse. If the database is slow or gone, an unbounded queue turns
     * a database problem into a memory problem and takes the module down with it - and a module
     * that dies loses far more than the entries it dropped. Past the bound the oldest entry is
     * dropped and the fact is logged, so a gap in the trail is itself recorded rather than silent.
     *
     * @par What this does not promise
     * Durability across a crash. Entries sit in memory for as long as it takes one insert to
     * complete, and a process killed in that window loses them. Making the trail survive that
     * would mean writing it synchronously, which is the cost this exists to avoid; an audit of
     * commands is not a ledger, and the trade is deliberate.
     *
     * @author jensvogt47\@gmail.com
     */
    class AuditWriter final {

      public:

        /**
         * @brief The one writer for this process.
         */
        static AuditWriter &instance();

        /**
         * @brief Queues one event, returning at once.
         *
         * @param event the event to store.
         */
        void Write(const Entity::EAD::AuditEvent &event);

        /**
         * @brief Stops the writer thread, draining what is already queued.
         */
        ~AuditWriter();

        AuditWriter(const AuditWriter &) = delete;
        AuditWriter &operator=(const AuditWriter &) = delete;

      private:

        AuditWriter();

        void Run();

        std::deque<Entity::EAD::AuditEvent> _queued;
        std::mutex _mutex;
        std::condition_variable _wakeup;
        std::thread _worker;
        bool _stopping{false};
        long _dropped{};
    };

}// namespace Euclid::Database
