// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <chrono>
#include <string>

namespace Euclid::Database {

    /**
     * @brief Whether this instance is the one to carry out a periodic job now.
     *
     * @par
     * A module runs as several instances - EQS up to four, ESM up to ten - and work that is about
     * the installation rather than about a request must happen once per interval, not once per
     * interval per instance. A recount of every queue is the example this was written for: ten
     * instances each grouping every message in the database every five minutes is ten times the
     * work for one answer, and they would all write the same numbers.
     *
     * @par
     * The claim is a single atomic update on a shared document, keyed by the job's name: the
     * instance that moves @c nextRunAt into the future is the one that ran it, and everyone else
     * is told no without waiting. There is no lock to release and nothing to clean up - an
     * instance that dies holding a claim simply means the job is skipped until @c nextRunAt comes
     * round again, which for a periodic correction is the right failure.
     *
     * @par
     * Not a leader election, and deliberately not: nothing here needs the same instance to keep
     * doing the job, only for one of them to.
     *
     * @param name     the job's name, unique across the installation, e.g. "eqs-recount-queues".
     * @param interval how long until it is due again.
     * @return true if the caller should do the work now.
     */
    bool ClaimPeriodicRun(const std::string &name, std::chrono::seconds interval);

}// namespace Euclid::Database
