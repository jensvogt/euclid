//
// Created by vogje01 on 9/1/26.
//

#pragma once

// C++ includes
#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace Euclid::Database::Entity::EAP {

    /**
     * @brief What an application's artifact needs to be started with.
     *
     * @par
     * Deliberately a handful of process shapes rather than a version matrix: the runtime only
     * decides which interpreter (if any) the artifact is handed to, so a JDK 17 and a JDK 25
     * application are both JAVA - which one runs is whichever "java" the application's own
     * command or PATH resolves to. BINARY covers anything already executable, which is where
     * C++ and Rust applications land.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    enum class Runtime {
        JAVA,
        PYTHON,
        NODEJS,
        BINARY,
        UNKNOWN
    };

    static std::map<Runtime, std::string> RuntimeNames{
            {Runtime::JAVA, "JAVA"},
            {Runtime::PYTHON, "PYTHON"},
            {Runtime::NODEJS, "NODEJS"},
            {Runtime::BINARY, "BINARY"},
            {Runtime::UNKNOWN, "UNKNOWN"},
    };

    [[maybe_unused]]
    static std::string RuntimeToString(const Runtime &runtime) {
        return RuntimeNames[runtime];
    }

    [[maybe_unused]]
    static Runtime RuntimeFromString(const std::string &runtime) {
        const auto it = std::ranges::find_if(RuntimeNames, [&runtime](const auto &pair) { return pair.second == runtime; });
        return it != RuntimeNames.end() ? it->first : Runtime::UNKNOWN;
    }

    /**
     * @brief The interpreter a runtime's artifact is handed to, and the arguments that precede
     * the artifact path.
     *
     * @par
     * Only used when an application does not spell out its own command. Resolved through PATH
     * rather than as absolute paths, so an installation picks whichever java/python3/node it
     * has - euclid does not manage language toolchains, it only launches processes.
     *
     * @param runtime the application's runtime.
     * @return interpreter and its leading arguments; empty for BINARY, which is its own command.
     */
    [[maybe_unused]]
    static std::vector<std::string> RuntimeCommandPrefix(const Runtime &runtime) {
        switch (runtime) {
            case Runtime::JAVA:
                return {"java", "-jar"};
            case Runtime::PYTHON:
                return {"python3"};
            case Runtime::NODEJS:
                return {"node"};
            case Runtime::BINARY:
            case Runtime::UNKNOWN:
                break;
        }
        return {};
    }

}// namespace Euclid::Database::Entity::EAP
