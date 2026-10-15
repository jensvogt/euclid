// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Euclid::Manager {

    /**
     * @brief Which node the next instance of an application goes on.
     *
     * @par Whose decision this is
     * The master's, and only the master's - see docs/worker-nodes.md §4. A worker never decides
     * that it should run something: the thresholds the pool size comes from are global, and a
     * worker evaluating them over its own share would make pool size a function of how many
     * workers exist.
     *
     * @par Deliberately dull
     * No bin-packing, no resource requests, no affinity beyond labels. Those are real features and
     * none of them is needed to get an application onto a second machine; adding them later does
     * not change the shape of this. What is here is §6, in order:
     *
     *   1. nodes that accept work, and whose labels satisfy the application's constraints
     *   2. of those, the one running fewest instances of *this* application per core, counting
     *      the one being placed
     *   3. tie broken by load average normalised by cpu count
     *   4. tie broken by node name
     *
     * And when a pool shrinks, ChooseToWithdraw() takes the slot back from the node that most
     * needs the relief.
     *
     * @par Why spread rather than pack
     * Step 2 is the one worth arguing about, and the argument is that the reason for a second
     * instance is almost always that the first is saturated. Packing the second onto the same node
     * puts it next to the thing that is already using that machine; spreading it is what the
     * autoscaler was asking for when it asked for another one.
     *
     * @par Why per core
     * Spread by a raw count treats every machine as the same size, and they are not: it dealt a
     * pool of six two apiece to a 128-core desktop and a 4-core Raspberry Pi. Per core, a pool
     * spreads in proportion to what each machine can carry. The cost is that a small pool may
     * now sit entirely on the largest machine - two instances on 128 cores are sparser than one on
     * four - which trades spreading for resilience against spreading for load, on purpose: a node
     * that dies is re-placed within a lease, an overloaded one stays overloaded.
     *
     * @par Why the last tie-break exists
     * So the answer is deterministic and a test can assert it. Two nodes with nothing between them
     * is the common case on a small installation - two idle machines, nothing running yet - and
     * "whichever the map iterated first" is not an answer anybody can reason about or reproduce.
     *
     * @author jensvogt47\@gmail.com
     */
    namespace Placement {

        /**
         * @brief One node placement may choose, and what it is being judged on.
         */
        struct Candidate {

            std::string name;

            std::map<std::string, std::string> labels;

            /**
             * @brief Cores, which the load average is divided by. A node reporting a load of 4 on
             * eight cores is half busy; the same figure on two cores is twice oversubscribed, and
             * comparing the raw numbers would send work to the smaller machine.
             */
            long cpuCount{};

            /**
             * @brief One-minute load average as the node last reported it.
             */
            double loadAverage{};

            /**
             * @brief How many instances of *the application being placed* this node already runs.
             * Not how many instances in total - see the note on spread above.
             */
            long instancesOfApplication{};

            /**
             * @brief Whether the node is live and not drained.
             *
             * @par
             * Asked of Entity::EAP::Node::acceptsWork() by the caller rather than recomputed here,
             * because liveness needs a clock and this needs none - which is what makes every case
             * below assertable without one.
             */
            bool acceptsWork{};
        };

        /**
         * @brief What an application will accept.
         *
         * @par
         * Worth having from the start, because the reason to add a worker is often that one
         * particular application needs one particular machine - a GPU, a licence dongle, a network
         * it can reach.
         */
        struct Constraints {

            /**
             * @brief Node names this application may run on. Empty means any.
             */
            std::vector<std::string> nodes;

            /**
             * @brief Labels a node must carry, all of them, with these values.
             */
            std::map<std::string, std::string> labels;
        };

        /**
         * @brief Whether one node satisfies an application's constraints.
         */
        [[nodiscard]] inline bool Satisfies(const Candidate &candidate, const Constraints &constraints) {

            if (!candidate.acceptsWork) return false;

            if (!constraints.nodes.empty() && !std::ranges::contains(constraints.nodes, candidate.name)) {
                return false;
            }

            // Every label, with the value named. A node carrying extra labels is fine - a
            // constraint says what an application needs, not what a node may be.
            return std::ranges::all_of(constraints.labels, [&candidate](const auto &required) {
                const auto found = candidate.labels.find(required.first);
                return found != candidate.labels.end() && found->second == required.second;
            });
        }

        /**
         * @brief How busy a node is, per core. Lower is better.
         */
        [[nodiscard]] inline double NormalisedLoad(const Candidate &candidate) {
            return candidate.loadAverage / static_cast<double>(std::max(1L, candidate.cpuCount));
        }

        /**
         * @brief Load-per-core figures closer together than this are the same load.
         *
         * @par
         * A tolerance rather than an exact comparison: these are doubles arriving from different
         * machines' /proc, and "equal enough to fall through to the next rule" is what makes the
         * answer reproducible instead of depending on the last bit of a float.
         */
        constexpr double kSameLoad = 0.01;

        /**
         * @brief Compares how densely two nodes would carry this application per core, with
         * @p addA and @p addB instances added to each: negative when @p a would be the sparser,
         * positive when @p b would, zero when they would be the same.
         *
         * @par
         * Cross-multiplied rather than divided, so equal densities compare equal exactly - three
         * instances on twelve cores and one on four are a tie, not a rounding error apart. A node
         * that reported no core count is taken as one core, pessimistic and therefore safe, as in
         * NormalisedLoad().
         */
        [[nodiscard]] inline long long CompareDensity(const Candidate &a, const long addA, const Candidate &b, const long addB) {
            const auto coresA = static_cast<long long>(std::max(1L, a.cpuCount));
            const auto coresB = static_cast<long long>(std::max(1L, b.cpuCount));
            return static_cast<long long>(a.instancesOfApplication + addA) * coresB
                   - static_cast<long long>(b.instancesOfApplication + addB) * coresA;
        }

        /**
         * @brief Picks the node the next instance should go on.
         *
         * @param candidates every node known to the account, judged or not.
         * @param constraints what the application will accept.
         * @return the node's name, or nothing when no node satisfies the constraints - which is a
         * slot that stays unplaced rather than one forced onto a machine that cannot run it.
         */
        [[nodiscard]] inline std::optional<std::string> Choose(const std::vector<Candidate> &candidates,
                                                               const Constraints &constraints = {}) {

            const Candidate *best = nullptr;
            for (const auto &candidate: candidates) {

                if (!Satisfies(candidate, constraints)) continue;
                if (best == nullptr) {
                    best = &candidate;
                    continue;
                }

                // Sparsest per core once this instance is on it first, then least loaded per core,
                // then the name. Each comparison only breaks a tie in the one before it, so the
                // order here is the policy and not an implementation detail.
                //
                // Per core, and counting the instance being placed. A raw count dealt a pool of six
                // out two apiece across a 128-core desktop, a 32-core box and a 4-core Raspberry
                // Pi, and the Pi - seven JVMs on four cores across all its applications - sat at a
                // load of 37 while the desktop idled. Weighed by cores, the pool fills machines in
                // proportion to their size; between machines of one size it is the plain count it
                // always was, since the core counts cancel.
                if (const auto density = CompareDensity(candidate, 1, *best, 1); density != 0) {
                    if (density < 0) best = &candidate;
                    continue;
                }

                if (const auto difference = NormalisedLoad(candidate) - NormalisedLoad(*best);
                    std::abs(difference) > kSameLoad) {
                    if (difference < 0) best = &candidate;
                    continue;
                }

                if (candidate.name < best->name) best = &candidate;
            }

            if (best == nullptr) return std::nullopt;
            return best->name;
        }

        /**
         * @brief Picks the node to give a slot back from when an application shrinks.
         *
         * @par
         * Placement's mirror, and the half that used to be missing: shrinking took whichever slot
         * was last in the list, wherever it ran, so an overloaded node kept its share however the
         * pool moved. In order:
         *
         *   1. a node that does not accept work - not live, or drained. Its slot is running
         *      nowhere or is meant to be leaving, and giving it back costs nothing that is working.
         *   2. the most loaded per core - shrinking is the one chance to relieve a busy machine
         *      without stopping anything that is needed
         *   3. the densest per core in this application
         *   4. the name, so the answer is reproducible
         *
         * @param holders the nodes holding a slot of this application, each with
         * instancesOfApplication its count. Constraints are not consulted: a slot that is already
         * placed is given back wherever it is.
         * @return the node's name, or nothing when @p holders holds no slot.
         */
        [[nodiscard]] inline std::optional<std::string> ChooseToWithdraw(const std::vector<Candidate> &holders) {

            const Candidate *best = nullptr;
            for (const auto &candidate: holders) {

                if (candidate.instancesOfApplication <= 0) continue;
                if (best == nullptr) {
                    best = &candidate;
                    continue;
                }

                if (candidate.acceptsWork != best->acceptsWork) {
                    if (!candidate.acceptsWork) best = &candidate;
                    continue;
                }

                if (const auto difference = NormalisedLoad(candidate) - NormalisedLoad(*best);
                    std::abs(difference) > kSameLoad) {
                    if (difference > 0) best = &candidate;
                    continue;
                }

                if (const auto density = CompareDensity(candidate, 0, *best, 0); density != 0) {
                    if (density > 0) best = &candidate;
                    continue;
                }

                if (candidate.name < best->name) best = &candidate;
            }

            if (best == nullptr) return std::nullopt;
            return best->name;
        }

        /**
         * @brief One instance to take off a node so that placement puts it back somewhere better.
         */
        struct Move {
            std::string from;
            std::string to;
        };

        /**
         * @brief Whether the pool is better off with one instance moved, and from where to where.
         *
         * @par
         * Placement is decided once, when a slot is made, and nothing reconsiders it: a pool that
         * shrank, or lost a node and got it back, keeps whatever shape the slots happened to end up
         * in. This is the reconsideration. The move it proposes is carried out by withdrawing one
         * slot from @c from - the replacement is then placed by Choose(), the same rule as every
         * other slot, and lands on @c to.
         *
         * @par
         * From, in order:
         *
         *   1. a holder that may not have the work any more - drained, not live, or no longer
         *      satisfying the application's constraints. Always moved off, if anywhere will take it.
         *   2. otherwise the holder densest per core in this application.
         *
         * @par
         * To is Choose()'s answer for the pool as it stands. A move is only proposed off a holder
         * that may keep its instance when it is a strict improvement: @c from carries more per core
         * than @c to would with one more (`n_from / c_from > (n_to + 1) / c_to`, cross-multiplied).
         * That one inequality does two jobs. It is what makes Choose() put the replacement on @c to
         * rather than back where it came from - after the withdrawal @c from would sit at
         * `n_from / c_from` against @c to's `(n_to + 1) / c_to` - and it is what makes the process
         * end: the densest holder only ever gets sparser, and the node it hands to stays below
         * where it was, so the sorted densities fall with every move and cannot cycle.
         *
         * @param candidates every node, as for Choose(), with instancesOfApplication its count - a
         *        holder with no node record left included, as not accepting work.
         * @param constraints what the application will accept.
         * @return the move, or nothing when no move would improve the pool.
         */
        [[nodiscard]] inline std::optional<Move> ChooseRebalance(const std::vector<Candidate> &candidates,
                                                                 const Constraints &constraints = {}) {

            const Candidate *from = nullptr;
            bool fromMustLeave = false;
            for (const auto &candidate: candidates) {

                if (candidate.instancesOfApplication <= 0) continue;
                const bool mustLeave = !Satisfies(candidate, constraints);

                if (from == nullptr) {
                    from = &candidate;
                    fromMustLeave = mustLeave;
                    continue;
                }
                if (mustLeave != fromMustLeave) {
                    if (mustLeave) {
                        from = &candidate;
                        fromMustLeave = true;
                    }
                    continue;
                }
                if (const auto density = CompareDensity(candidate, 0, *from, 0); density != 0) {
                    if (density > 0) from = &candidate;
                    continue;
                }
                if (candidate.name < from->name) from = &candidate;
            }
            if (from == nullptr) return std::nullopt;

            const auto chosen = Choose(candidates, constraints);
            if (!chosen.has_value() || *chosen == from->name) return std::nullopt;

            if (!fromMustLeave) {
                const auto to = std::ranges::find(candidates, *chosen, &Candidate::name);
                if (to == candidates.end()) return std::nullopt;
                // n_from / c_from > (n_to + 1) / c_to - see CompareDensity() for why it is
                // cross-multiplied rather than divided.
                if (CompareDensity(*from, 0, *to, 1) <= 0) return std::nullopt;
            }

            return Move{.from = from->name, .to = *chosen};
        }

    }// namespace Placement

}// namespace Euclid::Manager
