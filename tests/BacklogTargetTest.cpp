// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE BacklogTargetTest
#include <boost/test/unit_test.hpp>

// Euclid includes
#include <euclid/manager/BacklogTarget.h>
#include <euclid/manager/UtilisationSignals.h>

using Euclid::main::InstancesForBacklog;
using Euclid::main::IsSaturated;
using Euclid::main::IsWorking;
using Euclid::main::NextDesiredCount;

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

// ── One threshold answered two questions, and the pool went to its ceiling ──

BOOST_AUTO_TEST_CASE(AWorkingInstanceIsNotASaturatedOne) {

    // The measured case, 2026-09-18. Sixteen parser instances between 7.5% and 11.7% of a core,
    // mean backlog 0.38 messages each, pool pinned at its ceiling of 16. evaluateScaling() grows a
    // pool when EVERY running instance was busy - a genuine saturation signal when busy counts
    // concurrent requests, and a tautology when it means "used more than 5% of a core", because a
    // pool of working JVMs always does.
    //
    // The whole fix is that these two disagree in that band.
    for (const double utilisation: {7.5, 9.3, 11.7}) {
        BOOST_TEST(IsWorking(utilisation));
        BOOST_TEST(!IsSaturated(utilisation));
    }
}

BOOST_AUTO_TEST_CASE(AListenerBlockedOnIoStillCountsAsWorking) {

    // The other half, and why the low bar cannot simply be raised to the high one. A listener
    // waiting on storage and a database reports a few percent while holding thousands of messages;
    // if that read as idle, the pool's idle timer would run out underneath the instance doing the
    // work. Working has to stay cheap to satisfy - it is only saturation that had to get dear.
    BOOST_TEST(IsWorking(5.0));
    BOOST_TEST(IsWorking(6.2));
    BOOST_TEST(!IsWorking(4.9));
}

BOOST_AUTO_TEST_CASE(AFullInstanceIsBothWorkingAndSaturated) {

    // Saturation must imply working, or an instance at 90% would keep the idle timer running and
    // become eligible to be stopped at the moment it most needs to stay.
    for (const double utilisation: {75.0, 90.0, 100.0, 380.0}) {
        BOOST_TEST(IsSaturated(utilisation));
        BOOST_TEST(IsWorking(utilisation));
    }
}

BOOST_AUTO_TEST_CASE(NeverReportedIsNeitherWorkingNorSaturated) {

    // Utilisation is negative for "never reported", which is not the same as zero. Nothing is
    // known about such an instance; applyUnreportedAreBusy() handles it, and it must not fall out
    // of these two as an idle instance.
    BOOST_TEST(!IsWorking(-1.0));
    BOOST_TEST(!IsSaturated(-1.0));
}

// ── The target had no way down ─────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(TheTargetFollowsARealBacklogUpAtOnce) {

    // A backlog already costs latency, so there is nothing to be gained by approaching it slowly.
    BOOST_TEST(NextDesiredCount(3, 12, 3, 16) == 12);
    BOOST_TEST(NextDesiredCount(1, 16, 1, 16) == 16);
}

BOOST_AUTO_TEST_CASE(TheTargetComesBackDownOneInstanceAtATime) {

    // The bug. applyBacklog() only ever raised the target, leaving the descent to the idle branch,
    // which resets it to minInstances - but that branch only runs once the whole group has been
    // quiet for a minute, and a pool with a queue in front of it never is. So on an application
    // nothing lowered the target at all, and one raised during a genuine burst stayed raised for
    // the life of the process.
    BOOST_TEST(NextDesiredCount(16, 0, 3, 16) == 15);
    BOOST_TEST(NextDesiredCount(15, 0, 3, 16) == 14);
}

BOOST_AUTO_TEST_CASE(TheDescentReachesTheFloorAndStops) {

    // Walked all the way down, one tick at a time, and then held. minInstances is a promise, not a
    // starting point to fall through.
    int target = 16;
    for (int tick = 0; tick < 40; ++tick) target = NextDesiredCount(target, 0, 3, 16);

    BOOST_TEST(target == 3);
}

BOOST_AUTO_TEST_CASE(TheDescentDoesNotOvershootTheTarget) {

    // One above the target steps exactly onto it rather than one below, which would ask for fewer
    // instances than the backlog wants and then immediately ask for them back.
    BOOST_TEST(NextDesiredCount(9, 8, 3, 16) == 8);
    BOOST_TEST(NextDesiredCount(8, 8, 3, 16) == 8);
}

BOOST_AUTO_TEST_CASE(WorkArrivingDuringTheDescentStopsItAtOnce) {

    // The asymmetry is the point: giving instances up is reversible in a single tick, so a lull
    // part-way through a descent costs nothing. This is what makes stepping down safe where
    // slamming to minInstances was not.
    int target = NextDesiredCount(16, 0, 3, 16);
    target = NextDesiredCount(target, 0, 3, 16);
    BOOST_TEST(target == 14);

    BOOST_TEST(NextDesiredCount(target, 16, 3, 16) == 16);
}

BOOST_AUTO_TEST_CASE(TheTargetIsHeldInsideThePoolsOwnBounds) {

    // A backlog big enough to ask for a hundred instances still only gets the ceiling, and a floor
    // above the ceiling wins - minInstances is a promise and maxInstances only a limit on growth,
    // so a crossed configuration must not produce a pool smaller than it was told to keep.
    BOOST_TEST(NextDesiredCount(4, 100, 3, 16) == 16);
    BOOST_TEST(NextDesiredCount(4, 0, 5, 16) == 5);
    BOOST_TEST(NextDesiredCount(8, 100, 10, 4) == 10);
}

BOOST_AUTO_TEST_SUITE_END()
