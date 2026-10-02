// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <chrono>
#include <map>
#include <string>
#include <vector>

// MongoDB includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/document/value.hpp>

namespace Euclid::Database::Entity::EAP {

    using std::chrono::system_clock;

    /**
     * @brief A host that runs applications euclid placed there, and is not the manager's own.
     *
     * @par
     * Registered by a `euclid-wrk` rather than created by anybody: a worker announces itself,
     * renews, and goes quiet. The record is what the master has to go on in between, and what
     * `eap list-nodes` shows an operator. See docs/worker-nodes.md.
     *
     * @par What a node is not
     * Not a place euclid's own modules run - EQS, ESM, EAM and the rest stay on the manager's
     * host, which is a much larger problem and one nothing here depends on solving. Not a storage
     * node: it holds cached artifacts and log files and nothing durable. And not a scheduler - it
     * never decides how many instances exist or that it should run one.
     *
     * @author jensvogt47\@gmail.com
     */
    struct Node {

        /**
         * @brief ID
         */
        std::string oid;

        /**
         * @brief What this node calls itself, and the name every assignment is written against.
         *
         * @par
         * Chosen by the worker and unique within an account. A worker that restarts re-registers
         * under the same name and picks up the instances it already had, which is what makes
         * registration idempotent and a restart cheap rather than a re-placement.
         */
        std::string name;

        std::string accountId;

        /**
         * @brief The principal that registered this node, and the only one allowed to renew it.
         *
         * @par
         * Not in the proposal, and it has to be. §7 has a worker announce a name of its own
         * choosing, which on its own means any principal holding `eap:register-node` can register
         * under a name another worker already uses - and then receive that worker's instance
         * assignments and, through `issue-instance-credentials`, the application credentials that
         * go with them. That is one worker reading another's secrets, which is a larger hole than
         * anything the lease protects against.
         *
         * @par
         * So the first registration of a name binds it, and a registration under a name a
         * different principal holds is refused rather than taken over. An operator who really
         * means to move a node name deletes the registration first, which is a deliberate act and
         * leaves the leases alone - see IEapRepository::deleteNode.
         */
        std::string principal;

        /**
         * @brief Labels an application's placement constraints are matched against.
         *
         * @par
         * The reason to add a worker is often that one particular application needs one particular
         * machine - a GPU, a licence dongle, a network it can reach - so a node says what it is
         * and an application says what it needs. Free-form on purpose: euclid has no opinion about
         * what "gpu" means.
         */
        std::map<std::string, std::string> labels;

        /**
         * @brief How many CPUs the node has, which placement divides its load average by.
         */
        long cpuCount{};

        /**
         * @brief The euclid version the worker is built from, so a mismatch is visible before it
         * is mysterious.
         */
        std::string version;

        /**
         * @brief One-minute load average as the node last reported it.
         *
         * @par
         * Placement's third tie-break, normalised by @ref cpuCount - see §6. Reported by the
         * worker on every renewal rather than collected: the worker is the only thing that can
         * read this machine's /proc, and it is already making that call every tick.
         *
         * @par
         * Zero for a node that has never said, which reads as idle. That is the optimistic
         * direction and it is the right one: the figure only ever breaks a tie between nodes
         * already running the same number of instances of the application, so being wrong about it
         * costs one instance landing on a busier machine than it might have.
         */
        double loadAverage{};

        /**
         * @brief Whether this node still accepts new instances.
         *
         * @par
         * Set by `eap drain-node`, and the only field an operator writes. A drained node keeps
         * running what it has and renews its leases as usual - draining is not stopping - but
         * nothing new is placed on it, so its instances leave as they are replaced.
         */
        bool drained{false};

        /**
         * @brief When the worker last renewed.
         *
         * @par
         * The master's view of whether the node is there at all. Not the same thing as a lease,
         * which is per instance: this says when anybody last heard from the node, and a lease says
         * until when its claim on one slot is good. Both move on the same call, because the
         * heartbeat and the poll are deliberately one request - there is no way to be renewing and
         * not reconciling.
         */
        system_clock::time_point lastSeen;

        system_clock::time_point created = system_clock::now();
        system_clock::time_point modified = system_clock::now();

        /**
         * @brief Whether this node has been heard from recently enough to place work on.
         *
         * @par
         * Asked of the node, not of a lease. A node that has gone quiet is not a node whose
         * instances may be re-placed - that question is per instance and is answered by
         * @ref ModuleInstance::leaseExpiresAt, deliberately, because a worker that lost its
         * connection has not necessarily lost its processes. See worker-nodes.md §5.
         *
         * @param within how long silence is tolerated.
         * @param now the clock to compare against.
         */
        [[nodiscard]] bool isLive(const std::chrono::seconds within,
                                  const system_clock::time_point now = system_clock::now()) const {
            return lastSeen.time_since_epoch().count() != 0 && now <= lastSeen + within;
        }

        /**
         * @brief Whether a new instance may be placed here.
         */
        [[nodiscard]] bool acceptsWork(const std::chrono::seconds within,
                                       const system_clock::time_point now = system_clock::now()) const {
            return !drained && isLive(within, now);
        }

        [[nodiscard]] bsoncxx::document::value toDocument() const;

        [[nodiscard]] static Node fromDocument(const bsoncxx::document::view &doc);
    };

}// namespace Euclid::Database::Entity::EAP
