//
// Created by vogje01 on 8/31/26.
//

#pragma once

// C++ includes
#include <algorithm>
#include <map>
#include <string>

namespace Euclid::Database::Entity::ETS {

    /**
     * @brief State a transfer server should be in.
     *
     * @par
     * This is a *desired* state, not an observed one: the ETS module's start/stop actions do
     * nothing but write it, and euclid-mgr's reconciler is what makes reality match by
     * spawning or stopping the corresponding process. Whether a server is actually up is
     * reported separately, from the manager's own module pool - see EtsServer's "list-servers".
     *
     * @par
     * Keeping the two apart is what makes the arrangement survive a restart: the definition
     * and its desired state live in the database, so a manager that comes back up brings every
     * RUNNING server back with it, without ETS having to be reachable at that moment.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    enum class TransferServerState {
        RUNNING,
        STOPPED,
        UNKNOWN
    };

    static std::map<TransferServerState, std::string> TransferServerStateNames{
            {TransferServerState::RUNNING, "RUNNING"},
            {TransferServerState::STOPPED, "STOPPED"},
            {TransferServerState::UNKNOWN, "UNKNOWN"},
    };

    [[maybe_unused]]
    static std::string TransferServerStateToString(const TransferServerState &state) {
        return TransferServerStateNames[state];
    }

    [[maybe_unused]]
    static TransferServerState TransferServerStateFromString(const std::string &state) {
        const auto it = std::ranges::find_if(TransferServerStateNames, [&state](const auto &pair) { return pair.second == state; });
        return it != TransferServerStateNames.end() ? it->first : TransferServerState::UNKNOWN;
    }

}// namespace Euclid::Database::Entity::ETS
