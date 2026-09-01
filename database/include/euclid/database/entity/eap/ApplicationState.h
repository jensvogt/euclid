//
// Created by vogje01 on 9/1/26.
//

#pragma once

// C++ includes
#include <algorithm>
#include <map>
#include <string>

namespace Euclid::Database::Entity::EAP {

    /**
     * @brief State an application should be in.
     *
     * @par
     * A *desired* state, not an observed one: EAP's start/stop actions do nothing but write it,
     * and euclid-mgr's reconciler is what makes reality match by spawning or stopping the
     * application's processes. Whether an application is actually up is reported separately,
     * from the manager's own module pool - the same split transfer servers use, and for the same
     * reason: the definition survives in the database, so a manager coming back up restarts
     * everything that should be running without EAP having to be reachable at that moment.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    enum class ApplicationState {
        RUNNING,
        STOPPED,
        UNKNOWN
    };

    static std::map<ApplicationState, std::string> ApplicationStateNames{
            {ApplicationState::RUNNING, "RUNNING"},
            {ApplicationState::STOPPED, "STOPPED"},
            {ApplicationState::UNKNOWN, "UNKNOWN"},
    };

    [[maybe_unused]]
    static std::string ApplicationStateToString(const ApplicationState &state) {
        return ApplicationStateNames[state];
    }

    [[maybe_unused]]
    static ApplicationState ApplicationStateFromString(const std::string &state) {
        const auto it = std::ranges::find_if(ApplicationStateNames, [&state](const auto &pair) { return pair.second == state; });
        return it != ApplicationStateNames.end() ? it->first : ApplicationState::UNKNOWN;
    }

}// namespace Euclid::Database::Entity::EAP
