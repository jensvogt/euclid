// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE WorkerReconcilerTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <chrono>
#include <string>

// Euclid includes
#include <WorkerReconciler.h>

using Euclid::Worker::Reconciler::Assignment;
using Euclid::Worker::Reconciler::Decide;
using Euclid::Worker::Reconciler::Running;
using Euclid::Worker::Reconciler::State;
using namespace std::chrono_literals;

// The rule this file pins is the whole of docs/worker-nodes.md §5, which is the only reason running
// applications on a second machine is safe at all.
//
// The case it exists for cannot be reached by running the thing: a worker partitioned from the
// master for longer than its lease, with its processes still alive, while the master has already
// given that work to another node. Reproducing that against a live installation means breaking a
// network and waiting. As a decision over four inputs it is a handful of assertions.
//
// The asymmetry to keep in mind throughout: the worker stops at its deadline, the master waits
// until the deadline *plus* a skew margin. The gap is the window where neither is running the work
// - which is the direction this is allowed to be wrong in. Two of something that must run once is
// worse than none of it for one lease period.

namespace {

    const auto kNow = std::chrono::system_clock::now();

    Assignment assignment(const std::string &instanceId, const std::string &revision = "r1") {
        return Assignment{.instanceId = instanceId,
                          .runtimeName = "billing-000000000000-production",
                          .applicationId = "billing",
                          .revision = revision};
    }

    Running running(const std::string &instanceId, const std::string &revision = "r1") {
        return Running{.instanceId = instanceId,
                       .runtimeName = "billing-000000000000-production",
                       .revision = revision};
    }

}// namespace

// ── The lease, which comes before everything ────────────────────────────────

BOOST_AUTO_TEST_CASE(ALapsedLeaseStopsEverythingAndStartsNothing) {

    // The case the whole design is built around. The master will re-place this work the moment the
    // deadline passes, so the worker has to have let go of it by then - and it decides that on its
    // own clock, without needing to reach anybody.
    const State state{.assigned = {assignment("i-1"), assignment("i-2")},
                      .running = {running("i-1")},
                      .leaseExpiresAt = kNow - 1s,
                      .renewed = false};

    const auto plan = Decide(state, kNow);

    BOOST_TEST(plan.leaseLost);
    BOOST_TEST_REQUIRE(plan.stop.size() == 1U);
    BOOST_TEST(plan.stop.front().instanceId == "i-1");
    BOOST_TEST(plan.start.empty(), "a worker past its lease started something");
}

BOOST_AUTO_TEST_CASE(ALapsedLeaseOverridesWhatTheLastAssignmentSaid) {

    // Even with a full assignment in hand and nothing running, a worker past its deadline starts
    // nothing. The assignment it holds is stale by definition - it is older than the lease that
    // expired - and acting on it is how two nodes come to run one slot.
    const State state{.assigned = {assignment("i-1")},
                      .running = {},
                      .leaseExpiresAt = kNow - 1s,
                      .renewed = false};

    const auto plan = Decide(state, kNow);

    BOOST_TEST(plan.leaseLost);
    BOOST_TEST(plan.start.empty());
    BOOST_TEST(plan.stop.empty());
}

BOOST_AUTO_TEST_CASE(AFailedRenewalIsNotByItselfAReasonToStop) {

    // The mistake this design exists to avoid, and the reason there is a lease rather than a
    // heartbeat. A renewal that did not come back is a blip; with a 45-second lease against a
    // 10-second tick it is three chances to recover from something that would otherwise have cost
    // an outage. The worker carries on until the deadline, not until the first failure.
    const State state{.assigned = {assignment("i-1")},
                      .running = {running("i-1")},
                      .leaseExpiresAt = kNow + 30s,
                      .renewed = false};

    const auto plan = Decide(state, kNow);

    BOOST_TEST(!plan.leaseLost);
    BOOST_TEST(plan.stop.empty(), "a single failed renewal stopped a healthy instance");
    BOOST_TEST(plan.start.empty());
}

BOOST_AUTO_TEST_CASE(TheDeadlineIsExactWithNoMarginOnTheWorkersSide) {

    // The worker gets no grace, deliberately: the master's margin is what absorbs clock skew, and
    // a margin on both sides would overlap the two windows and put the work on two machines.
    const State onTime{.assigned = {assignment("i-1")}, .running = {running("i-1")}, .leaseExpiresAt = kNow, .renewed = true};
    const State past{.assigned = {assignment("i-1")}, .running = {running("i-1")}, .leaseExpiresAt = kNow - 1ms, .renewed = true};

    BOOST_TEST(!Decide(onTime, kNow).leaseLost);
    BOOST_TEST(Decide(past, kNow).leaseLost);
}

BOOST_AUTO_TEST_CASE(AWorkerThatNeverHeldALeaseDoesNotReadAsExpired) {

    // A zero deadline is a worker that has not registered yet, not one whose lease ran out. Read
    // the other way, a worker would announce that it had lost its lease before it ever had one.
    const State state{.assigned = {}, .running = {}, .leaseExpiresAt = {}, .renewed = false};

    const auto plan = Decide(state, kNow);

    BOOST_TEST(!plan.leaseLost);
    BOOST_TEST(plan.start.empty());
    BOOST_TEST(plan.stop.empty());
}

// ── Ordinary reconciliation ─────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(AnAssignedSlotThatIsNotRunningIsStarted) {

    const State state{.assigned = {assignment("i-1")}, .running = {}, .leaseExpiresAt = kNow + 45s, .renewed = true};

    const auto plan = Decide(state, kNow);

    BOOST_TEST_REQUIRE(plan.start.size() == 1U);
    BOOST_TEST(plan.start.front().instanceId == "i-1");
    BOOST_TEST(plan.stop.empty());
}

BOOST_AUTO_TEST_CASE(AnAssignedSlotAlreadyRunningIsLeftAlone) {

    // The common case, every tick, for ever. A plan that is not empty here would restart the
    // installation's applications on a timer.
    const State state{.assigned = {assignment("i-1")}, .running = {running("i-1")}, .leaseExpiresAt = kNow + 45s, .renewed = true};

    const auto plan = Decide(state, kNow);

    BOOST_TEST(plan.start.empty());
    BOOST_TEST(plan.stop.empty());
}

BOOST_AUTO_TEST_CASE(AnInstanceNoLongerAssignedIsStopped) {

    // How a re-placed slot is given up. The worker renewed successfully, the slot named another
    // node, so it came back absent - and the worker stops it without being told to. This is the
    // other half of the partition story: the first worker let go at its deadline, and if it comes
    // back it finds the slot gone and stops whatever it still had.
    const State state{.assigned = {}, .running = {running("i-1")}, .leaseExpiresAt = kNow + 45s, .renewed = true};

    const auto plan = Decide(state, kNow);

    BOOST_TEST(!plan.leaseLost, "an ordinary de-assignment was reported as a lost lease");
    BOOST_TEST_REQUIRE(plan.stop.size() == 1U);
    BOOST_TEST(plan.stop.front().instanceId == "i-1");
    BOOST_TEST(plan.start.empty());
}

BOOST_AUTO_TEST_CASE(ARevisionThatMovedOnIsReplaced) {

    // A redeploy. Instances are replaced, not migrated and not patched, so the one running the old
    // build stops and a new one starts in the same slot.
    const State state{.assigned = {assignment("i-1", "r2")},
                      .running = {running("i-1", "r1")},
                      .leaseExpiresAt = kNow + 45s,
                      .renewed = true};

    const auto plan = Decide(state, kNow);

    BOOST_TEST_REQUIRE(plan.stop.size() == 1U);
    BOOST_TEST(plan.stop.front().instanceId == "i-1");
    BOOST_TEST_REQUIRE(plan.start.size() == 1U);
    BOOST_TEST(plan.start.front().revision == "r2");
}

BOOST_AUTO_TEST_CASE(AMasterThatNamesNoRevisionDoesNotCauseARestart) {

    // An empty revision is a master that did not answer the question - an older one, or one whose
    // application record has no timestamp. Treating that as "different" would stop and start every
    // instance on every tick, which looks like a crash loop and is not one.
    const State state{.assigned = {assignment("i-1", "")},
                      .running = {running("i-1", "r1")},
                      .leaseExpiresAt = kNow + 45s,
                      .renewed = true};

    const auto plan = Decide(state, kNow);

    BOOST_TEST(plan.stop.empty(), "an unknown revision cycled a healthy instance");
    BOOST_TEST(plan.start.empty());
}

BOOST_AUTO_TEST_CASE(AReplacedInstanceIsNotStoppedTwice) {

    // The two passes - revision mismatch, then no-longer-assigned - must not both claim the same
    // instance, or the worker would try to kill a pid it has already reaped.
    const State state{.assigned = {assignment("i-1", "r2")},
                      .running = {running("i-1", "r1")},
                      .leaseExpiresAt = kNow + 45s,
                      .renewed = true};

    const auto plan = Decide(state, kNow);

    BOOST_TEST(plan.stop.size() == 1U);
}

BOOST_AUTO_TEST_CASE(SeveralSlotsAreDecidedIndependently) {

    const State state{.assigned = {assignment("i-1"), assignment("i-2", "r2"), assignment("i-4")},
                      .running = {running("i-1"), running("i-2", "r1"), running("i-3")},
                      .leaseExpiresAt = kNow + 45s,
                      .renewed = true};

    const auto plan = Decide(state, kNow);

    // i-1 unchanged, i-2 replaced, i-3 no longer assigned, i-4 new.
    BOOST_TEST(plan.start.size() == 2U);
    BOOST_TEST(plan.stop.size() == 2U);

    const auto starting = [&plan](const std::string &id) {
        return std::ranges::any_of(plan.start, [&id](const Assignment &a) { return a.instanceId == id; });
    };
    const auto stopping = [&plan](const std::string &id) {
        return std::ranges::any_of(plan.stop, [&id](const Running &r) { return r.instanceId == id; });
    };

    BOOST_TEST(starting("i-2"));
    BOOST_TEST(starting("i-4"));
    BOOST_TEST(stopping("i-2"));
    BOOST_TEST(stopping("i-3"));
    BOOST_TEST(!starting("i-1"));
    BOOST_TEST(!stopping("i-1"));
}
