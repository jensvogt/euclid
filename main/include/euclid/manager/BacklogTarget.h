// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

namespace Euclid::main {

    /**
     * @brief How many instances a reported backlog calls for.
     *
     * @par Why this is a header of its own
     * The manager is an executable rather than a library, so nothing in it can be linked into a
     * test. This is the arithmetic that decides how large a pool grows, it has been wrong twice,
     * and both times the fault was in the shape of the sum rather than anywhere a running system
     * made it obvious. Pulling it out is what lets it be checked.
     *
     * @par The mean, not the sum
     * Every instance reports the depth of the queues it polls, and whether those are shared decides
     * what adding them up means. A @QueueListener or @TopicListener puts every instance on one
     * queue, so all of them report the same number and the sum multiplies one queue by the size of
     * the pool - a signal that grows every time it is acted on. Observed: 563 messages stuck in one
     * shared queue read as 9,008 across sixteen instances. A @BucketListener gives each instance
     * its own queue, where the sum is right. Neither end can tell which it is, so the mean is used:
     * the true depth when the queue is shared, that instance's share when it is not.
     *
     * @par A target, not an increment
     * The answer is an absolute pool size, not "one more than now". An increment has no
     * equilibrium - scaling up satisfies it, so it asks again on the next tick - and with a shared
     * queue the mean does not fall as the pool grows, so nothing ever stops it short of
     * maxInstances. Dividing by the threshold gives the size at which each instance would hold
     * about a threshold's worth, and the pool settles there.
     *
     * @param pending messages waiting, summed over the instances that reported.
     * @param reporting how many instances that sum came from.
     * @param threshold messages per instance worth adding an instance for.
     * @return the pool size the backlog calls for, or 0 when it calls for nothing.
     */
    constexpr int InstancesForBacklog(const long pending, const long reporting, const long threshold) {

        // Nothing reported is not the same as nothing waiting: with no denominator there is no
        // mean, and guessing one would scale on a figure nobody sent.
        if (reporting <= 0 || pending <= 0 || threshold <= 0) return 0;

        if (const auto perInstance = pending / reporting; perInstance >= threshold) {
            // Rounded up, so a backlog of one and a half thresholds asks for two instances rather
            // than one - the remainder is real work and would otherwise never be counted.
            return static_cast<int>((perInstance + threshold - 1) / threshold);
        }
        return 0;
    }

}// namespace Euclid::main
