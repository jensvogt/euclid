//
// Created by vogje01 on 9/2/26.
//

#pragma once

// C++ includes
#include <functional>
#include <map>
#include <ranges>
#include <set>
#include <string>
#include <vector>

// Euclid includes
#include <euclid/core/LogStream.h>

namespace Euclid::main {

    /**
     * @brief Orders modules so that each is started after everything it depends on.
     *
     * @par
     * Before this existed, start order was whatever order the manager happened to hold its
     * services in - alphabetical, because they live in a std::map keyed by name - so a module that
     * called another during its own start-up was relying on the alphabet, and a rename could
     * silently break it. This makes the order something the configuration states.
     *
     * @par
     * The two interesting cases are the ones a running installation cannot be made to demonstrate.
     * A dependency naming a module that is not registered - inactive in euclid.json, or misspelled
     * - is reported and ignored: waiting for it would mean never starting, which is a worse answer
     * than starting without it. A cycle is reported and broken at the point it was entered, for
     * the same reason: refusing to start half an installation over a configuration mistake would
     * turn a misconfiguration into an outage.
     *
     * @par
     * Modules that depend on nothing keep the order they arrive in, so an installation that
     * declares no dependencies at all starts exactly as it did before.
     *
     * @param dependencies every module by name, each mapped to the names it depends on
     * @return the same names, each placed after everything it depends on that is present
     */
    inline std::vector<std::string> topologicalStartOrder(const std::map<std::string, std::vector<std::string> > &dependencies) {

        std::vector<std::string> ordered;
        ordered.reserve(dependencies.size());
        std::set<std::string> started;

        // Depth-first, so a module is emitted only after everything it named. "visiting" is what
        // turns a cycle into a diagnosis instead of a stack overflow: meeting a module already on
        // the current path means the configuration describes something no order can satisfy.
        std::set<std::string> visiting;
        std::function<void(const std::string &)> visit = [&](const std::string &name) {
            if (started.contains(name)) return;
            if (visiting.contains(name)) {
                log_error << "Module dependency cycle involving '" << name << "' - starting it anyway, in whatever order the cycle was entered; fix euclid.json";
                return;
            }
            visiting.insert(name);
            // at() rather than operator[]: the map is const, and every name reaching here is one
            // the guard below already established is in it.
            for (const auto &dependency: dependencies.at(name)) {
                if (!dependencies.contains(dependency)) {
                    log_warning << "Module '" << name << "' depends on '" << dependency << "', which is not an active module - ignoring that dependency";
                    continue;
                }
                visit(dependency);
            }
            visiting.erase(name);
            started.insert(name);
            ordered.push_back(name);
        };

        for (const auto &name: dependencies | std::views::keys) visit(name);
        return ordered;
    }

}// namespace Euclid::main