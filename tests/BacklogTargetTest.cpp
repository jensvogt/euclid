// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE BacklogTargetTest
#include <boost/test/unit_test.hpp>

// Euclid includes
#include <euclid/manager/BacklogTarget.h>

using Euclid::main::InstancesForBacklog;

// How large a pool grows when work is waiting. Wrong twice, both times in the shape of the sum
// rather than anywhere a running installation made it obvious, and both times the symptom was a
// pool sitting at maxInstances with nothing to do.
//
// The manager is an executable rather than a library, so nothing else in it can be reached from a
// test. This arithmetic was pulled into a header for exactly that reason.

namespace {
    constexpr long kThreshold = 100;
}

BOOST_AUTO_TEST_SUITE(BacklogTargetTest)

// ── The sum multiplied a shared queue by the size of the pool ───────────────

BOOST_AUTO_TEST_CASE(AShareQueueDoesNotGrowJustBecauseThePoolDid) {

    // The reported case. 563 messages stuck in one queue that every instance polls, so every
    // instance reports 563 and the old code added them up: 563 with one instance, 9,008 with
    // sixteen. The figure grew every time it was acted on, which is a signal justifying itself.
    //
    // The answer must be the same however many instances are reporting it, because it is the same
    // queue.
    const auto one = InstancesForBacklog(563, 1, kThreshold);
    const auto four = InstancesForBacklog(563 * 4, 4, kThreshold);
    const auto sixteen = InstancesForBacklog(563 * 16, 16, kThreshold);

    BOOST_TEST(one == four);
    BOOST_TEST(four == sixteen);
    BOOST_TEST(one == 6);// 563 over a threshold of 100, rounded up
}

BOOST_AUTO_TEST_CASE(ItAsksForASizeRatherThanOneMoreEachTime) {

    // An increment has no equilibrium: scaling up satisfies it, so it asks again on the next tick
    // and the pool walks to its ceiling whatever the backlog was. Feeding the answer back in has
    // to be a fixed point.
    const auto target = InstancesForBacklog(563, 1, kThreshold);

    // The pool is now that size, and every instance reports the same shared queue.
    const auto again = InstancesForBacklog(563 * target, target, kThreshold);

    BOOST_TEST(again == target);
}

BOOST_AUTO_TEST_CASE(AQueuePerInstanceIsStillAnsweredFromTheMean) {

    // A bucket listener gives each instance its own queue, so the sum is the real total. The mean
    // is then that instance's own share, which is what decides whether another instance would
    // help - four instances holding 200 each want two, not eight.
    BOOST_TEST(InstancesForBacklog(800, 4, kThreshold) == 2);
}

// ── Where it must say nothing ──────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(BelowTheThresholdAsksForNothing) {

    // Not "one instance" - nothing. Asking for a size at all would raise desiredCount and stop the
    // pool ever coming back to its floor.
    BOOST_TEST(InstancesForBacklog(99, 1, kThreshold) == 0);
    BOOST_TEST(InstancesForBacklog(99 * 8, 8, kThreshold) == 0);
}

BOOST_AUTO_TEST_CASE(NothingReportedIsNotNothingWaiting) {

    // With no denominator there is no mean, and guessing one scales on a figure nobody sent.
    BOOST_TEST(InstancesForBacklog(10000, 0, kThreshold) == 0);
}

BOOST_AUTO_TEST_CASE(AnEmptyQueueAsksForNothing) {
    BOOST_TEST(InstancesForBacklog(0, 4, kThreshold) == 0);
}

BOOST_AUTO_TEST_CASE(ANonsenseThresholdIsRefusedRatherThanDividedBy) {
    BOOST_TEST(InstancesForBacklog(1000, 1, 0) == 0);
}

// ── Rounding ───────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(TheRemainderCountsAsWork) {

    // One and a half thresholds is two instances' worth. Rounding down would leave the remainder
    // uncounted for ever, since it is never enough on its own to ask for anything.
    BOOST_TEST(InstancesForBacklog(150, 1, kThreshold) == 2);
    BOOST_TEST(InstancesForBacklog(100, 1, kThreshold) == 1);
    BOOST_TEST(InstancesForBacklog(101, 1, kThreshold) == 2);
}

BOOST_AUTO_TEST_SUITE_END()
