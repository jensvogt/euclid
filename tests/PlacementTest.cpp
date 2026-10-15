// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE PlacementTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <map>
#include <optional>
#include <string>
#include <vector>

// Euclid includes
#include <Placement.h>

using Euclid::Manager::Placement::Candidate;
using Euclid::Manager::Placement::Choose;
using Euclid::Manager::Placement::Constraints;
using Euclid::Manager::Placement::NormalisedLoad;

// Which node the next instance goes on - docs/worker-nodes.md §6, in order: eligible nodes, then
// fewest instances of this application per core, then least loaded per core, then the name. And
// which one gives a slot back when the pool shrinks.
//
// The last tie-break is there so the answer is deterministic and a test can assert it. Two idle
// machines with nothing running is the common case on a small installation, and "whichever the map
// iterated first" is not an answer anybody can reproduce or reason about - so the ordering is the
// policy, and these pin each step of it separately.

namespace {

    Candidate node(const std::string &name, const long instances = 0, const double load = 0.0,
                   const long cpus = 4, const bool accepts = true) {
        return Candidate{.name = name,
                         .labels = {},
                         .cpuCount = cpus,
                         .loadAverage = load,
                         .instancesOfApplication = instances,
                         .acceptsWork = accepts};
    }

}// namespace

// ── Eligibility ─────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(NoNodesMeansNoPlacement) {

    // A slot that stays unplaced, rather than one forced onto a machine that cannot run it.
    BOOST_TEST(!Choose({}).has_value());
}

BOOST_AUTO_TEST_CASE(ANodeThatDoesNotAcceptWorkIsNotChosen) {

    // Drained, or not heard from recently enough. Asked of the node by the caller rather than
    // recomputed here, so this rule needs no clock.
    const std::vector candidates{node("node-a", 0, 0.0, 4, false)};

    BOOST_TEST(!Choose(candidates).has_value());
}

BOOST_AUTO_TEST_CASE(OnlyNodesTheApplicationNamesAreConsidered) {

    // The reason to add a worker is often that one application needs one machine - a GPU, a
    // licence dongle, a network it can reach.
    const std::vector candidates{node("node-a"), node("node-b")};
    const Constraints only{.nodes = {"node-b"}, .labels = {}};

    BOOST_TEST((Choose(candidates, only) == std::optional<std::string>("node-b")));
}

BOOST_AUTO_TEST_CASE(ANamedNodeThatIsNotEligibleIsStillNotChosen) {

    // Naming a node is not an override: a drained node named by an application stays drained.
    const std::vector candidates{node("node-b", 0, 0.0, 4, false)};
    const Constraints only{.nodes = {"node-b"}, .labels = {}};

    BOOST_TEST(!Choose(candidates, only).has_value());
}

BOOST_AUTO_TEST_CASE(EveryRequiredLabelHasToMatch) {

    auto gpu = node("node-a");
    gpu.labels = {{"gpu", "true"}, {"region", "eu"}};

    auto plain = node("node-b");
    plain.labels = {{"region", "eu"}};

    const std::vector candidates{gpu, plain};

    BOOST_TEST((Choose(candidates, Constraints{.nodes = {}, .labels = {{"gpu", "true"}}}) == std::optional<std::string>("node-a")));

    // Both labels, and only one node has both.
    BOOST_TEST((Choose(candidates, Constraints{.nodes = {}, .labels = {{"gpu", "true"}, {"region", "eu"}}}) == std::optional<std::string>("node-a")));

    // A label nobody carries places nothing.
    BOOST_TEST(!Choose(candidates, Constraints{.nodes = {}, .labels = {{"fpga", "true"}}}).has_value());
}

BOOST_AUTO_TEST_CASE(ALabelWithTheWrongValueDoesNotMatch) {

    auto candidate = node("node-a");
    candidate.labels = {{"tier", "staging"}};

    BOOST_TEST(!Choose({candidate}, Constraints{.nodes = {}, .labels = {{"tier", "production"}}}).has_value());
}

BOOST_AUTO_TEST_CASE(ExtraLabelsOnANodeDoNotDisqualifyIt) {

    // A constraint says what an application needs, not what a node may be.
    auto candidate = node("node-a");
    candidate.labels = {{"gpu", "true"}, {"rack", "7"}, {"owner", "team-a"}};

    BOOST_TEST((Choose({candidate}, Constraints{.nodes = {}, .labels = {{"gpu", "true"}}}) == std::optional<std::string>("node-a")));
}

// ── Spread, which is the actual policy ──────────────────────────────────────

BOOST_AUTO_TEST_CASE(TheNodeRunningFewestOfThisApplicationWins) {

    // The one decision worth arguing about, and the argument is that the reason for a second
    // instance is almost always that the first is saturated. Packing it next to the thing already
    // using that machine is not what the autoscaler was asking for.
    const std::vector candidates{node("node-a", 2), node("node-b", 0), node("node-c", 1)};

    BOOST_TEST((Choose(candidates) == std::optional<std::string>("node-b")));
}

BOOST_AUTO_TEST_CASE(SpreadBeatsALowerLoadAverage) {

    // Deliberate ordering: instance count is consulted before load. A node that is idle but
    // already runs two of this application loses to a busier one running none.
    const std::vector candidates{node("node-a", 2, 0.0), node("node-b", 0, 3.5)};

    BOOST_TEST((Choose(candidates) == std::optional<std::string>("node-b")));
}

// ── Spread per core ─────────────────────────────────────────────────────────

namespace {

    // Places @p slots instances of one application one at a time, as reconcileNodeApplication does,
    // and returns how many each node ended up with.
    std::map<std::string, long> placeInTurn(std::vector<Candidate> candidates, const int slots) {
        std::map<std::string, long> placed;
        for (int slot = 0; slot < slots; ++slot) {
            const auto chosen = Choose(candidates);
            if (!chosen.has_value()) break;
            ++placed[*chosen];
            for (auto &candidate: candidates) {
                if (candidate.name == *chosen) ++candidate.instancesOfApplication;
            }
        }
        return placed;
    }
}// namespace

BOOST_AUTO_TEST_CASE(APoolDoesNotLandOnTheSmallestMachineAsOftenAsOnTheLargest) {

    // What happened: a pool of six dealt two apiece to a 128-core desktop, a 32-core box and a
    // 4-core Raspberry Pi. Across its applications the Pi carried seven JVMs and a load of 37,
    // while the desktop sat at 0.03 per core. Per core, the Pi is the last place an instance goes.
    const auto placed = placeInTurn({node("desktop", 0, 0.0, 128), node("nuc", 0, 0.0, 32), node("pios", 0, 0.0, 4)}, 6);

    BOOST_TEST(!placed.contains("pios"));
    BOOST_TEST(placed.at("desktop") > placed.at("nuc"));
}

BOOST_AUTO_TEST_CASE(APoolFillsMachinesInProportionToTheirCores) {

    // 32 : 8 : 1 by cores, and a pool of 41 instances lands exactly so.
    const auto placed = placeInTurn({node("large", 0, 0.0, 128), node("medium", 0, 0.0, 32), node("small", 0, 0.0, 4)}, 41);

    BOOST_TEST(placed.at("large") == 32);
    BOOST_TEST(placed.at("medium") == 8);
    BOOST_TEST(placed.at("small") == 1);
}

BOOST_AUTO_TEST_CASE(BetweenMachinesOfOneSizeItIsThePlainCount) {

    // The core counts cancel, so equal machines are dealt round-robin exactly as before.
    const auto placed = placeInTurn({node("node-a", 0, 0.0, 8), node("node-b", 0, 0.0, 8), node("node-c", 0, 0.0, 8)}, 6);

    BOOST_TEST(placed.at("node-a") == 2);
    BOOST_TEST(placed.at("node-b") == 2);
    BOOST_TEST(placed.at("node-c") == 2);
}

BOOST_AUTO_TEST_CASE(EqualDensityIsATieThatLoadBreaks) {

    // With the next instance counted, two on twelve cores becomes three in twelve and none on four
    // becomes one in four - exactly equal, cross-multiplied rather than rounded. So load decides,
    // and the busier machine loses.
    const std::vector candidates{node("twelve", 2, 6.0, 12), node("four", 0, 0.1, 4)};

    BOOST_TEST((Choose(candidates) == std::optional<std::string>("four")));
}

// ── Giving a slot back ──────────────────────────────────────────────────────

using Euclid::Manager::Placement::ChooseToWithdraw;

BOOST_AUTO_TEST_CASE(NothingHeldMeansNothingToWithdraw) {

    BOOST_TEST(!ChooseToWithdraw({}).has_value());
    BOOST_TEST(!ChooseToWithdraw({node("node-a", 0)}).has_value());
}

BOOST_AUTO_TEST_CASE(TheMostLoadedNodeGivesBackFirst) {

    // The Pi again: one instance there at 9 per core against five on an idle desktop. Shrinking is
    // the one chance to relieve it without stopping anything that is needed, and it used to take
    // whichever slot was last in the list instead.
    const std::vector holders{node("desktop", 5, 3.7, 128), node("pios", 1, 37.3, 4)};

    BOOST_TEST((ChooseToWithdraw(holders) == std::optional<std::string>("pios")));
}

BOOST_AUTO_TEST_CASE(ANodeThatDoesNotAcceptWorkGivesBackBeforeABusyOne) {

    // Not live, or drained: its slot is running nowhere or is meant to be leaving, so giving it
    // back costs nothing that is working - even against a machine at full load.
    const std::vector holders{node("busy", 1, 40.0, 4), node("drained", 1, 0.0, 4, false)};

    BOOST_TEST((ChooseToWithdraw(holders) == std::optional<std::string>("drained")));
}

BOOST_AUTO_TEST_CASE(AtEqualLoadTheDensestGivesBack) {

    const std::vector holders{node("sparse", 1, 0.0, 16), node("dense", 3, 0.0, 4)};

    BOOST_TEST((ChooseToWithdraw(holders) == std::optional<std::string>("dense")));
}

BOOST_AUTO_TEST_CASE(TheNameDecidesARemainingWithdrawTie) {

    const std::vector holders{node("node-c", 1), node("node-a", 1), node("node-b", 1)};

    BOOST_TEST((ChooseToWithdraw(holders) == std::optional<std::string>("node-a")));
}

// ── Load, normalised ────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(TheLeastLoadedPerCoreBreaksATie) {

    const std::vector candidates{node("node-a", 1, 3.0, 4), node("node-b", 1, 1.0, 4)};

    BOOST_TEST((Choose(candidates) == std::optional<std::string>("node-b")));
}

BOOST_AUTO_TEST_CASE(LoadIsPerCoreNotAbsolute) {

    // A load of 4 on eight cores is half busy; the same figure on two cores is twice
    // oversubscribed. Comparing the raw numbers would send work to the smaller machine.
    const std::vector candidates{node("big", 1, 4.0, 16), node("small", 1, 2.0, 2)};

    BOOST_TEST(NormalisedLoad(candidates[0]) < NormalisedLoad(candidates[1]));
    BOOST_TEST((Choose(candidates) == std::optional<std::string>("big")));
}

BOOST_AUTO_TEST_CASE(ANodeReportingNoCpuCountIsNotDividedByZero) {

    // A node registered by something that did not send cpuCount, or an older worker. Treated as
    // one core, which is pessimistic and therefore safe.
    const std::vector candidates{node("node-a", 0, 2.0, 0)};

    BOOST_TEST(NormalisedLoad(candidates.front()) == 2.0);
    BOOST_TEST((Choose(candidates) == std::optional<std::string>("node-a")));
}

// ── The name, so the answer is reproducible ─────────────────────────────────

BOOST_AUTO_TEST_CASE(TheNameBreaksARemainingTie) {

    // Two idle machines, nothing running - the common case on a small installation. Without this
    // the answer depends on iteration order, which nobody can reason about or reproduce.
    const std::vector candidates{node("node-c"), node("node-a"), node("node-b")};

    BOOST_TEST((Choose(candidates) == std::optional<std::string>("node-a")));
}

BOOST_AUTO_TEST_CASE(TheChoiceDoesNotDependOnTheOrderGiven) {

    // The property the name tie-break buys, stated directly: the same set of nodes placed in any
    // order gives the same answer.
    const auto a = node("node-a", 1, 0.5, 4);
    const auto b = node("node-b", 1, 0.5, 4);
    const auto c = node("node-c", 1, 0.5, 4);

    BOOST_TEST((Choose({a, b, c}) == std::optional<std::string>("node-a")));
    BOOST_TEST((Choose({c, b, a}) == std::optional<std::string>("node-a")));
    BOOST_TEST((Choose({b, c, a}) == std::optional<std::string>("node-a")));
}

BOOST_AUTO_TEST_CASE(LoadsWithinATolerenceFallThroughToTheName) {

    // These arrive from two different machines' /proc as doubles. "Equal enough to fall through to
    // the name" is what keeps the answer reproducible rather than dependent on the last bit of a
    // float - two genuinely equally-idle nodes should not swap places between ticks.
    const std::vector candidates{node("node-b", 0, 0.500), node("node-a", 0, 0.503)};

    BOOST_TEST((Choose(candidates) == std::optional<std::string>("node-a")));
}

BOOST_AUTO_TEST_CASE(ALoadDifferenceLargerThanTheToleranceStillDecides) {

    const std::vector candidates{node("node-a", 0, 1.0), node("node-b", 0, 0.5)};

    BOOST_TEST((Choose(candidates) == std::optional<std::string>("node-b")));
}

// ── Re-placement after a lease expires ─────────────────────────────────────

BOOST_AUTO_TEST_CASE(TheNodeThatLostTheLeaseNeedsNoSpecialCase) {

    // What ServiceController::reconcileNodeLeases leans on, and the reason it has no "exclude the
    // previous holder" branch: a lease only expires because the node stopped renewing, and a node
    // that stopped renewing is not live - so it is already not a candidate. The slot it held still
    // counts against it, which is why it would otherwise have been the obvious choice.
    auto lapsed = node("node-b", 1, 0.0, 4, false);// held the slot, no longer live
    const auto healthy = node("node-c", 0, 2.0);   // busier, but there

    const std::vector candidates{lapsed, healthy};

    BOOST_TEST((Choose(candidates) == std::optional<std::string>("node-c")));
}

BOOST_AUTO_TEST_CASE(ANodeThatCameBackCanKeepItsOwnSlot) {

    // The other half: a node that failed to renew in time but is live again is eligible, and with
    // the slot still counted against it it may or may not win - here it is the only candidate, so
    // it does. The master then extends rather than moves, and the worker - which stopped the
    // process on its own clock - starts it again on its next tick.
    const std::vector candidates{node("node-b", 1, 0.0, 4, true)};

    BOOST_TEST((Choose(candidates) == std::optional<std::string>("node-b")));
}

BOOST_AUTO_TEST_CASE(ASlotWhoseConstraintsNobodySatisfiesStaysPut) {

    // No eligible node means no placement, which the master reads as "leave it alone and say so".
    // Forcing it somewhere, or quietly taking it over locally, would be running an application
    // somewhere it said it must not - and naming a node or a label is exactly an application
    // saying that.
    auto onlyNode = node("node-b", 0, 0.0, 4, false);
    onlyNode.labels = {{"gpu", "true"}};

    BOOST_TEST(!Choose({onlyNode}, Constraints{.nodes = {}, .labels = {{"gpu", "true"}}}).has_value());
}

// ── All of it together ──────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(TheWholeRuleInOrder) {

    auto drained = node("node-a", 0, 0.0, 4, false);// eligible? no
    auto wrongLabel = node("node-b", 0, 0.0);       // labels? no
    auto packed = node("node-c", 2, 0.0);           // eligible, labelled, but runs two already
    auto busy = node("node-d", 1, 3.9);             // eligible, labelled, one instance, busy
    auto idle = node("node-e", 1, 0.1);             // eligible, labelled, one instance, idle

    for (auto *candidate: {&drained, &packed, &busy, &idle}) candidate->labels = {{"gpu", "true"}};

    const std::vector candidates{drained, wrongLabel, packed, busy, idle};

    BOOST_TEST((Choose(candidates, Constraints{.nodes = {}, .labels = {{"gpu", "true"}}}) == std::optional<std::string>("node-e")));
}

// ── Rebalancing ─────────────────────────────────────────────────────────────

using Euclid::Manager::Placement::ChooseRebalance;

namespace {

    // What the controller does with a move: withdraw one slot from `from`, then place the
    // replacement with Choose(). Returns where it landed.
    std::string applyMove(std::vector<Candidate> &candidates, const std::string &from, const Constraints &constraints = {}) {
        for (auto &candidate: candidates) {
            if (candidate.name == from) --candidate.instancesOfApplication;
        }
        const auto placed = Choose(candidates, constraints);
        BOOST_TEST_REQUIRE(placed.has_value());
        for (auto &candidate: candidates) {
            if (candidate.name == *placed) ++candidate.instancesOfApplication;
        }
        return *placed;
    }

    long countOn(const std::vector<Candidate> &candidates, const std::string &name) {
        for (const auto &candidate: candidates) {
            if (candidate.name == name) return candidate.instancesOfApplication;
        }
        return -1;
    }
}// namespace

BOOST_AUTO_TEST_CASE(ABalancedPoolIsLeftAlone) {

    // Nothing to gain, so nothing restarted: a move costs a running instance its work.
    BOOST_TEST(!ChooseRebalance({node("a", 2, 0.0, 8), node("b", 2, 0.0, 8)}).has_value());
    BOOST_TEST(!ChooseRebalance({node("a", 2, 0.0, 8), node("b", 1, 0.0, 8)}).has_value());
    BOOST_TEST(!ChooseRebalance({}).has_value());
}

BOOST_AUTO_TEST_CASE(AnInstanceLeftOnTheSmallMachineMovesToTheLargeOne) {

    // The Pi again: after a scale-down the one instance it still holds is a quarter of its cores,
    // against a desktop where one more would be a sixty-fourth of its.
    auto candidates = std::vector{node("desktop", 1, 0.0, 128), node("pios", 1, 0.0, 4)};

    const auto move = ChooseRebalance(candidates);
    BOOST_TEST_REQUIRE(move.has_value());
    BOOST_TEST(move->from == "pios");
    BOOST_TEST(move->to == "desktop");

    // And the replacement does land there - the rule's own guarantee, checked through Choose().
    BOOST_TEST(applyMove(candidates, move->from) == "desktop");
    BOOST_TEST(countOn(candidates, "pios") == 0);
}

BOOST_AUTO_TEST_CASE(AnUnevenPoolOnEqualMachinesEvensOut) {
    auto candidates = std::vector{node("a", 3, 0.0, 8), node("b", 1, 0.0, 8)};

    const auto move = ChooseRebalance(candidates);
    BOOST_TEST_REQUIRE(move.has_value());
    BOOST_TEST(move->from == "a");
    applyMove(candidates, move->from);

    BOOST_TEST(countOn(candidates, "a") == 2);
    BOOST_TEST(countOn(candidates, "b") == 2);
    BOOST_TEST(!ChooseRebalance(candidates).has_value());
}

BOOST_AUTO_TEST_CASE(ADrainedNodeIsMovedOffEvenWhenItIsTheSparsest) {

    // Drained means "give your work back", so its instance leaves whatever the arithmetic says.
    auto drained = node("old", 1, 0.0, 64, false);
    const auto move = ChooseRebalance({drained, node("new", 3, 0.0, 4)});

    BOOST_TEST_REQUIRE(move.has_value());
    BOOST_TEST(move->from == "old");
    BOOST_TEST(move->to == "new");
}

BOOST_AUTO_TEST_CASE(AHolderTheApplicationNoLongerAcceptsIsMovedOff) {

    // The application's labels changed under a running instance: where it runs is no longer
    // somewhere it may.
    auto gpu = node("gpu-box", 0, 0.0, 8);
    gpu.labels = {{"gpu", "true"}};
    const Constraints needsGpu{.nodes = {}, .labels = {{"gpu", "true"}}};

    const auto move = ChooseRebalance({node("plain", 1, 0.0, 64), gpu}, needsGpu);

    BOOST_TEST_REQUIRE(move.has_value());
    BOOST_TEST(move->from == "plain");
    BOOST_TEST(move->to == "gpu-box");
}

BOOST_AUTO_TEST_CASE(NothingMovesWhenNowhereElseWouldTakeIt) {

    // Drained, but the only other node does not satisfy the constraints either: withdrawing would
    // leave the pool short with nowhere to put the replacement.
    auto drained = node("old", 1, 0.0, 8, false);
    BOOST_TEST(!ChooseRebalance({drained}).has_value());
}

BOOST_AUTO_TEST_CASE(RebalancingAlwaysSettles) {

    // Every pool, moved one instance at a time as the controller does, reaches a state with no
    // move left - and never more moves than it has instances times nodes. A rule that could cycle
    // would restart instances for ever; the inequality is what rules that out.
    const std::vector<long> cores{1, 2, 4, 8, 32, 128};
    unsigned seed = 7;
    const auto next = [&seed](const unsigned bound) {
        seed = seed * 1103515245u + 12345u;
        return (seed >> 8) % bound;
    };

    for (int pool = 0; pool < 500; ++pool) {
        std::vector<Candidate> candidates;
        long instances = 0;
        const auto nodes = 2 + next(4);
        for (unsigned i = 0; i < nodes; ++i) {
            const long count = static_cast<long>(next(6));
            instances += count;
            candidates.push_back(node("node-" + std::to_string(i), count, 0.0, cores[next(cores.size())]));
        }

        long moves = 0;
        while (const auto move = ChooseRebalance(candidates)) {
            const auto landed = applyMove(candidates, move->from);
            BOOST_TEST(landed == move->to);
            BOOST_TEST_REQUIRE(++moves <= instances * static_cast<long>(nodes));
        }

        long after = 0;
        for (const auto &candidate: candidates) after += candidate.instancesOfApplication;
        BOOST_TEST(after == instances);
    }
}
