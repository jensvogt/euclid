// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <algorithm>
#include <chrono>
#include <ranges>
#include <string>
#include <vector>

namespace Euclid::Worker {

    /**
     * @brief What a worker should do on this tick: start these, stop those, or stop everything.
     *
     * @par Why this is a pure function
     * It is the whole of docs/worker-nodes.md §5, and §5 is the only reason this design is safe to
     * run. Everything around it - a gateway call, a fork, a file written - is plumbing that can be
     * exercised by running the thing. What cannot be exercised by running it is the case the rule
     * exists for: a worker that has been partitioned from the master for longer than its lease,
     * whose processes are still alive, while the master has already given its work to somebody
     * else. Reproducing that against a live installation means breaking a network and waiting; as
     * a decision over four inputs it is six lines of test.
     *
     * @par The rule, stated once
     * A worker renews on every tick, and a renewal is also the poll for what it should be running.
     * When the renewal does not come back, the worker does **not** keep running on the assumption
     * that the master will sort it out. It stops its own instances when its own lease runs out - a
     * local clock against a local deadline, needing nobody's agreement - because the master will
     * re-place that work the moment the lease has passed, and two of something that must run once
     * is worse than none of it for a lease period.
     *
     * @author jensvogt47\@gmail.com
     */
    namespace Reconciler {

        /**
         * @brief One slot the master says this node should be running.
         */
        struct Assignment {

            std::string instanceId;
            std::string runtimeName;
            std::string applicationId;

            /**
             * @brief What the master says the application is, now.
             *
             * @par
             * Carried so a redeploy is a restart rather than a re-download nothing acts on: an
             * instance running a revision that is no longer the current one has to be replaced,
             * and comparing them is the only way the worker can tell.
             */
            std::string revision;

            /**
             * @brief What starting it takes: the artifact to fetch and the runtime to hand it to.
             *
             * @par
             * §7 describes a renewal as answering (instanceId, applicationId, revision), which is
             * what deciding takes. Acting takes these as well, and the master sends them in the
             * same answer rather than making a worker ask per instance per tick for something it
             * has already read.
             *
             * @par
             * None of it is part of the decision in Decide(), which is why it sits here as data
             * the plan carries rather than anything the rule looks at.
             */
            std::string bucketErn;
            std::string artifactKey;
            long artifactSize{};
            std::string runtime;
            /**
             * @brief The application's own command, when it names one instead of its runtime's
             * interpreter. Empty for the ordinary case - see Worker::CommandLine().
             */
            std::string command;
            std::vector<std::string> arguments;
        };

        /**
         * @brief One process this worker currently has.
         */
        struct Running {
            std::string instanceId;
            std::string runtimeName;

            /**
             * @brief The revision this process was started with.
             */
            std::string revision;
        };

        /**
         * @brief What this tick asks of the worker.
         */
        struct Plan {

            /**
             * @brief Slots to start, because they are assigned and are not running.
             */
            std::vector<Assignment> start;

            /**
             * @brief Instances to stop: no longer assigned, or running the wrong revision.
             */
            std::vector<Running> stop;

            /**
             * @brief Whether this is the lease having run out rather than an ordinary difference.
             *
             * @par
             * When set, `stop` is everything the worker has and `start` is empty, and the reason
             * is not that the master said so - it is that the worker can no longer prove it is
             * allowed to run anything. Separated from an ordinary stop because it is worth saying
             * out loud in a log: an operator looking at a worker that shut down wants to know
             * whether it was told to or whether it lost contact.
             */
            bool leaseLost{false};
        };

        /**
         * @brief What the worker knows when it decides.
         */
        struct State {

            /**
             * @brief What the last successful renewal said to run. Empty when none has succeeded.
             */
            std::vector<Assignment> assigned;

            /**
             * @brief What this worker actually has running.
             */
            std::vector<Running> running;

            /**
             * @brief When this worker's own lease runs out, as the last successful renewal said.
             *
             * @par
             * The master's figure, held locally and compared against a local clock. Not re-asked
             * when a renewal fails, which is the point: the deadline a worker acts on has to be
             * one it already has, or a worker that cannot reach the master could never learn that
             * it must stop.
             */
            std::chrono::system_clock::time_point leaseExpiresAt{};

            /**
             * @brief Whether the most recent renewal succeeded.
             *
             * @par
             * A failed renewal is not by itself a reason to stop anything - that is the mistake
             * this design is built to avoid, and it is why the lease exists at all. A worker keeps
             * running through a failed renewal and stops only when the deadline passes. With a
             * 45-second lease and a 10-second tick that is three chances to recover from a blip
             * that would otherwise have cost an outage.
             */
            bool renewed{false};
        };

        /**
         * @brief Decides what to do.
         *
         * @param state what the worker knows.
         * @param now the worker's own clock.
         * @return the plan. When the lease has run out, everything stops and nothing starts.
         */
        [[nodiscard]] inline Plan Decide(const State &state, const std::chrono::system_clock::time_point now) {

            Plan plan;

            // The lease first, before anything else is considered. A worker past its deadline may
            // not start work and may not keep work, whatever the last assignment it heard said -
            // and whatever it would otherwise have concluded from comparing the two lists.
            //
            // No margin here, deliberately, and the asymmetry with the master's margin is the
            // safety property: the worker stops at the deadline and the master waits until the
            // deadline *plus* skew. The gap between the two is the window in which neither is
            // running the work, which is the direction this is allowed to be wrong in.
            const bool haveLease = state.leaseExpiresAt.time_since_epoch().count() != 0;
            if (haveLease && now > state.leaseExpiresAt) {
                plan.leaseLost = true;
                plan.stop = state.running;
                return plan;
            }

            // A worker that has never held a lease has nothing assigned and nothing to run, which
            // falls out of the comparison below - but it must not be read as "the lease expired",
            // or a worker would shut down before it ever registered.

            for (const auto &assignment: state.assigned) {
                const auto found = std::ranges::find_if(state.running, [&assignment](const Running &candidate) {
                    return candidate.instanceId == assignment.instanceId;
                });

                if (found == state.running.end()) {
                    plan.start.push_back(assignment);
                    continue;
                }

                // Running, but not the build the master now names. Replaced rather than left: an
                // instance is not migrated, and a revision that moved on is the one case where
                // something running correctly still has to go.
                //
                // Only when the master actually said what the revision is. An empty one is a
                // master that did not answer the question, and restarting on that would cycle
                // every instance on every tick.
                if (!assignment.revision.empty() && found->revision != assignment.revision) {
                    plan.stop.push_back(*found);
                    plan.start.push_back(assignment);
                }
            }

            // Anything running that is no longer assigned. This is how a re-placed slot is given
            // up: the worker renewed, the slot named another node, so it is absent from what came
            // back - and the worker stops it without needing to be told to.
            for (const auto &running: state.running) {
                const bool assigned = std::ranges::any_of(state.assigned, [&running](const Assignment &candidate) {
                    return candidate.instanceId == running.instanceId;
                });
                const bool alreadyStopping = std::ranges::any_of(plan.stop, [&running](const Running &candidate) {
                    return candidate.instanceId == running.instanceId;
                });
                if (!assigned && !alreadyStopping) plan.stop.push_back(running);
            }

            return plan;
        }

    }// namespace Reconciler

}// namespace Euclid::Worker
