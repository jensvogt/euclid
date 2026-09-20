// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/20/26.
//

#pragma once

// C++ includes
#include <set>
#include <string>
#include <vector>

namespace Euclid::main {

    /**
     * @brief Which application data directories a reconcile pass should remove.
     *
     * @par
     * Deleting an application used to leave its data directory behind for good - one per
     * application ever deleted, and a new application of the same name would start up on a dead
     * one's files. That second effect is the only reason runtime names carry a random suffix
     * (Database::Entity::GenerateRuntimeName), so removing the directory is what makes the suffix
     * unnecessary rather than load-bearing.
     *
     * @par Why the pools cannot answer this
     * The obvious place to notice was the orphaned-pool list in reconcileApplications: a pool this
     * controller still runs that EAP no longer defines. That is wrong, and wrong in the case that
     * matters. Stopping an application already deregisters its pool, so by the time somebody
     * deletes a stopped application there is no pool left to orphan - and stop it, check, then
     * delete is how anybody removes an application. Hooked there, the cleanup fired only for an
     * application deleted while it was still running.
     *
     * @par
     * So the question is asked of names rather than of pools: everything this controller has ever
     * registered as an application, against everything EAP defines now. A name in the first and
     * not the second has been deleted, whether or not it was running at the time.
     *
     * @par The empty-definitions guard
     * `defined` comes from one unguarded repository read. If that ever answers "no applications"
     * because it failed rather than because there are none, every remembered name looks deleted at
     * once - and this decides a recursive delete. An installation losing every application between
     * two reconciles is not a thing that happens; a database read failing is. So a pass that finds
     * nothing defined while remembering more than one pool removes nothing and waits for the next
     * one. Deleting the last application still cleans up, because that leaves exactly one name.
     *
     * @param known runtime names this controller has registered as applications
     * @param defined runtime names EAP currently defines
     * @return the names whose directories should be removed, in the order given
     */
    inline std::vector<std::string> DepartedApplicationPools(const std::set<std::string> &known,
                                                             const std::set<std::string> &defined) {

        if (defined.empty() && known.size() > 1) return {};

        std::vector<std::string> departed;
        for (const auto &name: known) {
            if (!defined.contains(name)) departed.push_back(name);
        }
        return departed;
    }

}// namespace Euclid::main
