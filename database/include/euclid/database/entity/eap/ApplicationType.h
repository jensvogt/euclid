// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 10/9/26.
//

#pragma once

// C++ includes
#include <algorithm>
#include <map>
#include <string>

namespace Euclid::Database::Entity::EAP {

    /**
     * @brief What kind of thing an application is, and therefore what finishing means.
     *
     * @par
     * Everything euclid ran until now was a PROCESS: it is started, it is expected to stay up, and
     * an exit is a fault to be restarted from. That assumption is spread across the reconciler and
     * the worker rather than written down anywhere - a slot below minInstances is re-placed, and
     * WorkerClient::Reap() reports *any* exit as CRASHED and schedules a backoff. For something
     * that is supposed to end, both of those are wrong: a nightly import that completes would be
     * restarted forever, each run reported as a crash.
     *
     * @par
     * A JOB is the other kind. It is started on demand - or, later, on a cron schedule - runs to
     * completion, and is done: exit 0 is success and not a restart, the slot is not re-placed, and
     * the autoscaler leaves it alone because a backlog says nothing about how many copies of a
     * one-shot task to run.
     *
     * @par
     * PROCESS is the default, deliberately: every application that existed before this field did
     * is one, and a definition read back without a type has to keep behaving exactly as it did.
     *
     * @author jensvogt47\@gmail.com
     */
    enum class ApplicationType {

        /**
         * @brief Started and kept up. An exit is a fault; the pool is held at its instance count.
         */
        PROCESS,

        /**
         * @brief Started to do one thing and finish. Exit 0 is success, and nothing restarts it.
         */
        JOB,

        /**
         * @brief What an unrecognised value reads as. Treated as PROCESS wherever behaviour is
         * decided, so a definition written by a newer euclid does not stop running under an older
         * one - see IsJob().
         */
        UNKNOWN
    };

    static std::map<ApplicationType, std::string> ApplicationTypeNames{
            {ApplicationType::PROCESS, "PROCESS"},
            {ApplicationType::JOB, "JOB"},
            {ApplicationType::UNKNOWN, "UNKNOWN"},
    };

    [[maybe_unused]]
    static std::string ApplicationTypeToString(const ApplicationType &type) {
        return ApplicationTypeNames[type];
    }

    [[maybe_unused]]
    static ApplicationType ApplicationTypeFromString(const std::string &type) {
        const auto it = std::ranges::find_if(ApplicationTypeNames, [&type](const auto &pair) { return pair.second == type; });
        return it != ApplicationTypeNames.end() ? it->first : ApplicationType::UNKNOWN;
    }

    /**
     * @brief Whether this type runs to completion, which is the only question any caller asks.
     *
     * @par
     * Written as "is it a JOB" rather than "is it not a PROCESS" so that UNKNOWN - a value from a
     * newer euclid, or a document somebody edited - keeps the long-running behaviour that every
     * definition had before this field existed. The failure of guessing wrong in that direction is
     * an application that stays up when it should have stopped; the other way round it is a
     * service that quietly stops being restarted.
     */
    [[maybe_unused]]
    static bool IsJob(const ApplicationType &type) {
        return type == ApplicationType::JOB;
    }

}// namespace Euclid::Database::Entity::EAP
