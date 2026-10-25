// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

namespace Euclid::main {

    /**
     * @brief What a reported utilisation figure means for scaling.
     *
     * @par Two questions, not one
     * A percentage is asked two different things by the autoscaler and they need two different
     * answers. "Is this instance doing anything" decides whether the pool's idle timer may run,
     * and a listener waiting on storage and a database while holding thousands of messages has to
     * answer yes at a very low figure. "Is this instance full enough that another would help"
     * decides whether to start one, and there the same low figure is meaningless.
     *
     * @par What one threshold for both did
     * It read every working instance as a saturated one. evaluateScaling() grows a pool when
     * *every* running instance was busy, which is a real saturation signal when busy comes from
     * activeRequests - that counts concurrent work. An application answers no gateway request, so
     * its only busy signal is this percentage, and at a 5% bar sixteen JVMs each using 9% of a
     * core all report busy, every tick, forever. "Every instance is busy" and "every instance is
     * running" became the same statement, so the pool grew to maxInstances and the same threshold
     * kept `lastActivityAt` fresh, which is the only thing that lets it shrink again.
     *
     * Measured on the parser pool: 16 of 16 instances between 7.5% and 11.7%, mean backlog 0.38
     * messages per instance, pool pinned at its ceiling of 16 with `InstancesForBacklog()`
     * correctly asking for nothing.
     *
     * @par Why this is a header of its own
     * The manager is an executable rather than a library, so nothing in it can be linked into a
     * test. The same reason `BacklogTarget.h` exists, and the same lesson: this is a rule that was
     * wrong in a way no running system made obvious, because a pool at its ceiling under load
     * looks like a pool that needs to be.
     */

    /**
     * @brief Utilisation, in percent, at or above which an instance counts as doing something.
     *
     * Low on purpose - it answers "is there anything going on here", and a listener that spends
     * its time blocked on I/O is working hard at a few percent of a core. Used only for the idle
     * timer, never to decide that another instance is wanted.
     */
    inline constexpr double kWorkingUtilisationPercent = 5.0;

    /**
     * @brief Utilisation, in percent, at or above which an instance is full enough to want help.
     *
     * High on purpose, and the whole point of the split. Below this an instance has headroom, and
     * starting a sibling to share work it could have done itself buys nothing and costs a JVM.
     */
    inline constexpr double kSaturatedUtilisationPercent = 75.0;

    /**
     * @brief Whether a reported utilisation means the instance is doing something at all.
     *
     * @param utilisation percent of a core, or negative for "never reported".
     */
    constexpr bool IsWorking(const double utilisation) {
        // Negative is the "never reported" marker, which is not the same as zero: an instance
        // nothing is known about must not read as an idle one.
        return utilisation >= kWorkingUtilisationPercent;
    }

    /**
     * @brief Whether a reported utilisation means another instance would help.
     *
     * @param utilisation percent of a core, or negative for "never reported".
     */
    constexpr bool IsSaturated(const double utilisation) {
        return utilisation >= kSaturatedUtilisationPercent;
    }

    /**
     * @brief Whether the requests a module instance is serving at once mean another would help.
     *
     * @par
     * The same question as IsSaturated(), for a euclid module, whose load is the gateway requests
     * it is handed rather than a percentage. An instance serves as many at once as it has request
     * threads, so it is full when every thread is taken - not when it has served a request at all.
     *
     * @par What "any request" did
     * Every module pool flapped between one instance and two, all the time. A module answering a
     * steady trickle - lease renewals, load reports, route refreshes - receives at least one
     * request in every one-second tick, and with one instance that alone made "every running
     * instance was busy" true. Three such ticks in a row is all the sustained-saturation guard asks
     * for, so a second instance was started; the next tick one of the two had nothing, the pool
     * was over its target, and the newcomer was stopped again. On 2026-10-25 esm, eap, ekv and ens
     * did this continuously, each instance serving one request at a time out of 8 to 32 threads.
     *
     * @param active the requests counted as load that the instance is serving right now.
     * @param threads the instance's request threads; anything below one is treated as one.
     */
    constexpr bool RequestsSaturate(const int active, const int threads) {
        return active >= (threads < 1 ? 1 : threads);
    }

}// namespace Euclid::main
