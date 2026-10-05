// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE NodeLeaseTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <chrono>
#include <string>

// Euclid includes
#include <euclid/database/Database.h>
#include <euclid/database/entity/eap/Node.h>
#include <euclid/database/entity/emm/Module.h>
#include <euclid/database/repository/eap/MongoEapRepository.h>
#include <euclid/database/repository/emm/MongoEmmRepository.h>

using Euclid::Database::MongoEapRepository;
using Euclid::Database::Entity::ModuleInstance;
using Euclid::Database::Entity::EAP::Node;
using namespace std::chrono_literals;

// Assignment is a lease, and that is the whole safety argument - docs/worker-nodes.md §5.
//
// The obvious design is for the master to notice a worker has gone quiet and start its instances
// somewhere else, which is the classic way to end up running two of something that must only run
// once: a worker that lost its connection has not necessarily lost its processes. The network
// broke; the JVM is still consuming the queue.
//
// So the guarantee runs the other way round. The worker renews every tick, and a worker that cannot
// renew stops its own instances when its own lease runs out - a local clock against a local
// deadline, needing nobody's agreement. The master re-places only after that deadline has passed
// plus a margin for skew. At any moment after expiry, either the worker has already stopped the
// instances or the worker is not executing at all; both are safe to re-place on top of.
//
// Every one of these is about a boundary where getting it wrong means two processes doing one job.

namespace {

    constexpr auto kAccount = "000000000000";
    constexpr auto kNode = "node-b";

    const auto kNow = std::chrono::system_clock::now();

    ModuleInstance leasedUntil(const std::chrono::system_clock::time_point expiry, const std::string &node = kNode) {
        ModuleInstance instance;
        instance.instanceId = "i-1";
        instance.assignedTo = node;
        instance.leaseExpiresAt = expiry;
        return instance;
    }

    MongoEapRepository freshRepository() {
        Euclid::Database::Database::instance().initializeMemory();
        return MongoEapRepository{};
    }

    Node nodeOf(const std::string &name = kNode) {
        Node node;
        node.name = name;
        node.accountId = kAccount;
        node.cpuCount = 4;
        node.version = "1.2.0";
        node.os = "linux";
        node.arch = "aarch64";
        node.labels = {{"gpu", "true"}};
        return node;
    }

}// namespace

// ── The lease ───────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(ALeaseWithTimeLeftHasNotExpired) {

    BOOST_TEST(!leasedUntil(kNow + 45s).leaseHasExpired(0s, kNow));
}

BOOST_AUTO_TEST_CASE(ALeaseThatRanOutHasExpired) {

    BOOST_TEST(leasedUntil(kNow - 1s).leaseHasExpired(0s, kNow));
}

BOOST_AUTO_TEST_CASE(TheMarginHoldsAJustExpiredLease) {

    // The master waits out the deadline *plus* a margin for clock skew between the node that wrote
    // it and itself. Without it, two clocks a second apart are enough for the master to re-place a
    // slot the worker still believes it holds - which is precisely the double-run this design
    // exists to prevent.
    const auto instance = leasedUntil(kNow - 2s);

    BOOST_TEST(instance.leaseHasExpired(0s, kNow));
    BOOST_TEST(!instance.leaseHasExpired(10s, kNow));
}

BOOST_AUTO_TEST_CASE(ASlotThatNeverHadALeaseHasNotExpired) {

    // An instance the manager runs itself holds no lease at all. Reading "no deadline" as
    // "deadline passed" would make every ordinary instance on a single-host installation look
    // re-placeable, which is the one reading that breaks an installation with no workers in it.
    ModuleInstance ordinary;
    ordinary.instanceId = "i-1";

    BOOST_TEST(ordinary.leaseExpiresAt.time_since_epoch().count() == 0);
    BOOST_TEST(!ordinary.leaseHasExpired(0s, kNow));
    BOOST_TEST(!ordinary.leaseHasExpired(60s, kNow + 24h));
}

// ── Whose slot it is ────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(ASlotIsItsAssignedNodesToRun) {

    BOOST_TEST(leasedUntil(kNow + 45s).isAssignedTo(kNode));
    BOOST_TEST(!leasedUntil(kNow + 45s).isAssignedTo("node-c"));
}

BOOST_AUTO_TEST_CASE(AnUnassignedSlotBelongsToTheManager) {

    // Empty means the manager's own, exactly as an empty host does, and for the same reason: it is
    // what every record written before workers existed carries. A worker must not read one as its
    // own, and the manager must not stop reading them as its own.
    ModuleInstance ordinary;
    ordinary.instanceId = "i-1";

    BOOST_TEST(ordinary.isAssignedTo(""));
    BOOST_TEST(!ordinary.isAssignedTo(kNode));
}

// ── The node record ─────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(ANodeHeardFromRecentlyIsLive) {

    auto node = nodeOf();
    node.lastSeen = kNow - 5s;

    BOOST_TEST(node.isLive(45s, kNow));
    BOOST_TEST(node.acceptsWork(45s, kNow));
}

BOOST_AUTO_TEST_CASE(ANodeThatHasGoneQuietIsNotLive) {

    auto node = nodeOf();
    node.lastSeen = kNow - 60s;

    BOOST_TEST(!node.isLive(45s, kNow));
}

BOOST_AUTO_TEST_CASE(ANodeNeverHeardFromIsNotLive) {

    // Registered but never renewed. Distinguished from "heard from at the epoch", which is what a
    // zero timestamp would otherwise compare as - and that reads as live for any tolerance once
    // the comparison is a subtraction.
    const auto node = nodeOf();

    BOOST_TEST(node.lastSeen.time_since_epoch().count() == 0);
    BOOST_TEST(!node.isLive(45s, kNow));
    BOOST_TEST(!node.acceptsWork(45s, kNow));
}

BOOST_AUTO_TEST_CASE(ADrainedNodeIsStillLiveButTakesNoWork) {

    // Draining is not stopping. The node keeps running what it has and keeps renewing - its
    // instances leave as they are replaced - because a node that stopped its work the moment it
    // was drained would make draining an outage, which is the thing it exists to avoid.
    auto node = nodeOf();
    node.lastSeen = kNow - 5s;
    node.drained = true;

    BOOST_TEST(node.isLive(45s, kNow));
    BOOST_TEST(!node.acceptsWork(45s, kNow));
}

// ── Assignment, renewal and reporting ───────────────────────────────────────

namespace {

    constexpr auto kModule = "billing-000000000000-production";

    Euclid::Database::MongoEmmRepository freshModules() {
        Euclid::Database::Database::instance().initializeMemory();
        return Euclid::Database::MongoEmmRepository{};
    }

    void seedSlot(Euclid::Database::MongoEmmRepository &repo, const std::string &instanceId) {
        Euclid::Database::Entity::Module module;
        module.name = kModule;
        module.executable = "/usr/local/euclid/bin/java";

        ModuleInstance instance;
        instance.instanceId = instanceId;
        instance.state = Euclid::Database::Entity::ModuleState::STOPPED;
        repo.upsertInstance(module, instance);
    }

    std::optional<ModuleInstance> slot(const Euclid::Database::MongoEmmRepository &repo, const std::string &instanceId) {
        for (const auto &module: repo.findAll()) {
            if (module.name != kModule) continue;
            for (const auto &instance: module.instances) {
                if (instance.instanceId == instanceId) return instance;
            }
        }
        return std::nullopt;
    }

}// namespace

BOOST_AUTO_TEST_CASE(AssigningASlotWritesTheNodeAndTheDeadline) {

    auto repo = freshModules();
    seedSlot(repo, "i-1");

    const auto expiry = kNow + 45s;
    BOOST_TEST_REQUIRE(repo.assignInstance(kModule, "i-1", kNode, expiry));

    const auto stored = slot(repo, "i-1");
    BOOST_TEST_REQUIRE(stored.has_value());
    BOOST_TEST(stored->assignedTo == kNode);
    BOOST_TEST(stored->isAssignedTo(kNode));
    BOOST_TEST(!stored->leaseHasExpired(0s, kNow));
}

BOOST_AUTO_TEST_CASE(AssigningASlotThatDoesNotExistSaysSo) {

    auto repo = freshModules();

    BOOST_TEST(!repo.assignInstance(kModule, "no-such-slot", kNode, kNow + 45s));
}

BOOST_AUTO_TEST_CASE(RenewingExtendsOnlyThisNodesSlots) {

    auto repo = freshModules();
    seedSlot(repo, "i-1");
    seedSlot(repo, "i-2");
    BOOST_TEST_REQUIRE(repo.assignInstance(kModule, "i-1", kNode, kNow + 10s));
    BOOST_TEST_REQUIRE(repo.assignInstance(kModule, "i-2", "node-c", kNow + 10s));

    BOOST_TEST(repo.renewInstanceLeases(kNode, kNow + 120s) == 1L);

    const auto mine = slot(repo, "i-1");
    const auto theirs = slot(repo, "i-2");
    BOOST_TEST_REQUIRE(mine.has_value());
    BOOST_TEST_REQUIRE(theirs.has_value());

    // Mine moved out; theirs did not.
    BOOST_TEST(!mine->leaseHasExpired(0s, kNow + 60s));
    BOOST_TEST(theirs->leaseHasExpired(0s, kNow + 60s));
}

BOOST_AUTO_TEST_CASE(AWorkerThatComesBackLateRenewsNothing) {

    // The partition case, and the reason no extra rule is needed for it. The worker was away long
    // enough for its lease to lapse; the master re-placed the slot onto another node. The worker
    // returns and renews - and extends nothing, because the slot no longer names it. It then finds
    // the slot absent from its desired set and stops the process, which is exactly right.
    auto repo = freshModules();
    seedSlot(repo, "i-1");
    BOOST_TEST_REQUIRE(repo.assignInstance(kModule, "i-1", kNode, kNow - 60s));

    // The master re-places it after the lease expired.
    BOOST_TEST_REQUIRE(repo.assignInstance(kModule, "i-1", "node-c", kNow + 45s));

    BOOST_TEST(repo.renewInstanceLeases(kNode, kNow + 120s) == 0L);

    const auto stored = slot(repo, "i-1");
    BOOST_TEST_REQUIRE(stored.has_value());
    BOOST_TEST(stored->assignedTo == "node-c", "the returning worker reclaimed a re-placed slot");
}

BOOST_AUTO_TEST_CASE(RenewingForNobodyRenewsNothing) {

    // An empty node name would otherwise match every slot the manager runs itself and put a lease
    // on instances that hold none.
    auto repo = freshModules();
    seedSlot(repo, "i-1");

    BOOST_TEST(repo.renewInstanceLeases("", kNow + 120s) == 0L);

    const auto stored = slot(repo, "i-1");
    BOOST_TEST_REQUIRE(stored.has_value());
    BOOST_TEST(stored->leaseExpiresAt.time_since_epoch().count() == 0, "an unassigned slot was given a lease");
}

BOOST_AUTO_TEST_CASE(ANodeReportsWhatItIsRunning) {

    auto repo = freshModules();
    seedSlot(repo, "i-1");
    BOOST_TEST_REQUIRE(repo.assignInstance(kModule, "i-1", kNode, kNow + 45s));

    BOOST_TEST_REQUIRE(repo.reportInstanceFromNode(kModule, "i-1", kNode, "192.168.178.29", 4242, 9000,
                                                   Euclid::Database::Entity::ModuleState::RUNNING));

    const auto stored = slot(repo, "i-1");
    BOOST_TEST_REQUIRE(stored.has_value());
    BOOST_TEST(stored->host == "192.168.178.29");
    BOOST_TEST(stored->pid == 4242);
    BOOST_TEST(stored->httpPort == 9000);
    BOOST_TEST((stored->state == Euclid::Database::Entity::ModuleState::RUNNING));

    // The assignment is untouched by a report: where it is running is the worker's to say, whose
    // slot it is is not.
    BOOST_TEST(stored->assignedTo == kNode);
}

BOOST_AUTO_TEST_CASE(ANodeCannotReportOnASlotThatIsNotItsOwn) {

    // Not a formality. A worker whose lease lapsed and whose slot was re-placed must not be able
    // to overwrite the record of the process that replaced it - that would leave the master
    // pointing at a pid on the wrong machine, which is the failure the host field exists to stop.
    auto repo = freshModules();
    seedSlot(repo, "i-1");
    BOOST_TEST_REQUIRE(repo.assignInstance(kModule, "i-1", "node-c", kNow + 45s));

    BOOST_TEST(!repo.reportInstanceFromNode(kModule, "i-1", kNode, "192.168.178.29", 4242, 9000,
                                            Euclid::Database::Entity::ModuleState::RUNNING));

    const auto stored = slot(repo, "i-1");
    BOOST_TEST_REQUIRE(stored.has_value());
    BOOST_TEST(stored->pid == -1, "a report from the wrong node was written anyway");
    BOOST_TEST(stored->host.empty());
}

BOOST_AUTO_TEST_CASE(ANodeCannotReportOnAnUnassignedSlot) {

    // The manager's own instances. A worker reporting onto one would be claiming a process it did
    // not start.
    auto repo = freshModules();
    seedSlot(repo, "i-1");

    BOOST_TEST(!repo.reportInstanceFromNode(kModule, "i-1", kNode, "192.168.178.29", 4242, 9000,
                                            Euclid::Database::Entity::ModuleState::RUNNING));
}

// ── What the repository keeps ───────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(ARegisteredNodeIsReadBackWhole) {

    auto repo = freshRepository();
    auto node = nodeOf();
    std::ignore = repo.upsertNode(node);

    const auto stored = repo.findNodeByName(kAccount, kNode);
    BOOST_TEST_REQUIRE(stored.has_value());
    BOOST_TEST(stored->name == kNode);
    BOOST_TEST(stored->cpuCount == 4L);
    BOOST_TEST(stored->version == "1.2.0");
    BOOST_TEST(stored->os == "linux");
    BOOST_TEST(stored->arch == "aarch64");
    BOOST_TEST_REQUIRE(stored->labels.contains("gpu"));
    BOOST_TEST(stored->labels.at("gpu") == "true");
}

BOOST_AUTO_TEST_CASE(RegisteringTwiceIsOneNode) {

    // What a restarted worker does. It has to come back as the node it was, or it abandons the
    // instances it is still running and the master places them somewhere else on top.
    auto repo = freshRepository();
    auto first = nodeOf();
    std::ignore = repo.upsertNode(first);

    auto second = nodeOf();
    second.version = "1.3.0";
    std::ignore = repo.upsertNode(second);

    const auto nodes = repo.listNodes(kAccount);
    BOOST_TEST_REQUIRE(nodes.size() == 1U);
    BOOST_TEST(nodes.front().version == "1.3.0");
}

BOOST_AUTO_TEST_CASE(RenewingDoesNotUndoADrain) {

    // The race this is shaped around: a worker renews every tick, and an operator drains between
    // two of them. A renewal that rewrote the document from what the worker last registered would
    // put `drained` back to false, and the drain would appear to have silently not happened.
    auto repo = freshRepository();
    auto node = nodeOf();
    std::ignore = repo.upsertNode(node);

    BOOST_TEST_REQUIRE(repo.setNodeDrained(kAccount, kNode, true));
    BOOST_TEST_REQUIRE(repo.touchNode(kAccount, kNode, kNow, 0.75));

    const auto stored = repo.findNodeByName(kAccount, kNode);
    BOOST_TEST_REQUIRE(stored.has_value());
    BOOST_TEST(stored->drained, "a renewal undid the drain");
    BOOST_TEST(stored->isLive(45s, kNow));

    // And the load average the renewal carried, which placement's third tie-break reads. It
    // travels on the heartbeat because that is the same call and the same tick.
    BOOST_TEST(stored->loadAverage == 0.75);
}

BOOST_AUTO_TEST_CASE(RenewingANodeThatIsNotRegisteredSaysSo) {

    // A worker whose registration was removed has to register again rather than carry on renewing
    // into nothing.
    auto repo = freshRepository();

    BOOST_TEST(!repo.touchNode(kAccount, "node-never-registered", kNow, 0.0));
}

BOOST_AUTO_TEST_CASE(NodesAreScopedToTheirAccount) {

    auto repo = freshRepository();
    auto mine = nodeOf();
    std::ignore = repo.upsertNode(mine);

    BOOST_TEST(repo.listNodes(kAccount).size() == 1U);
    BOOST_TEST(repo.listNodes("999999999999").empty());
    BOOST_TEST(!repo.findNodeByName("999999999999", kNode).has_value());
}

BOOST_AUTO_TEST_CASE(DeletingANodeRemovesOnlyTheRegistration) {

    // Deliberately says nothing about the instances assigned to it. They are the lease's business:
    // removing a registration must not become a way to re-place work on top of processes that are
    // still running.
    auto repo = freshRepository();
    auto node = nodeOf();
    std::ignore = repo.upsertNode(node);

    BOOST_TEST(repo.deleteNode(kAccount, kNode));
    BOOST_TEST(!repo.findNodeByName(kAccount, kNode).has_value());
    BOOST_TEST(!repo.deleteNode(kAccount, kNode), "deleting a node that is gone reported success");
}
