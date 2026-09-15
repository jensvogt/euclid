// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <memory>
#include <string>

namespace Euclid::Database {

    /**
     * @brief This process's slot in its pool, as the manager named it.
     *
     * @par
     * Stable across restarts of the same slot, unlike a pid: "which instance holds this" has to
     * survive the instance dying, and a restarted slot gets a new pid but the same instance id.
     * It is what `emm_module.instances[]` is keyed by.
     *
     * @par
     * A process started by hand rather than by the manager still gets something unique, so two of
     * them do not mistake each other's work for their own.
     *
     * @return the instance name.
     */
    const std::string &InstanceName();

    /**
     * @brief One outstanding piece of work the manager cannot see.
     *
     * @par What it is for
     * An `--async` command is answered at once and carried out on a detached thread. From outside
     * the process that thread does not exist: the autoscaler stops an instance it sees no requests
     * on, and the work stops with it, mid-purge or mid-resend. This is how a module says "I am
     * still doing something" - `evaluateScaling` leaves an instance alone while its
     * `backgroundTasks` is above zero.
     *
     * @par Why it is a guard rather than a pair of calls
     * The count has to come back down on *every* path out of the thread - the normal end, an early
     * return, a lost claim, an exception. ESM had four such paths and got three of them; the
     * fourth, a background delete by key, incremented a counter that was never reported at all, so
     * that one command never protected its instance. A destructor cannot be forgotten.
     *
     * @par How to hold one
     * Take it with Begin() before starting the thread and capture the shared_ptr in the thread's
     * lambda, so the count is already up when the request returns and comes down when the lambda
     * is destroyed:
     *
     * @code
     * const auto work = BackgroundWork::Begin("ens");
     * std::thread([work, ern] { ... }).detach();
     * @endcode
     *
     * @author jensvogt47\@gmail.com
     */
    class BackgroundWork final {

      public:

        /**
         * @brief Registers one outstanding task and tells the manager straight away.
         *
         * @param moduleName the module reporting, e.g. "esm".
         * @return a handle that releases the task when the last copy of it is destroyed.
         */
        static std::shared_ptr<BackgroundWork> Begin(std::string moduleName);

        /**
         * @brief Releases the task and reports the new count.
         */
        ~BackgroundWork();

        BackgroundWork(const BackgroundWork &) = delete;
        BackgroundWork &operator=(const BackgroundWork &) = delete;
        BackgroundWork(BackgroundWork &&) = delete;
        BackgroundWork &operator=(BackgroundWork &&) = delete;

      private:

        explicit BackgroundWork(std::string moduleName);

        std::string _moduleName;
    };

}// namespace Euclid::Database
