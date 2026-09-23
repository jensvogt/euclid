// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

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
     * Mostly process shapes rather than a version matrix: the runtime decides which interpreter
     * (if any) the artifact is handed to. BINARY covers anything already executable, which is
     * where C++ and Rust applications land.
     *
     * @par Why Java is versioned and nothing else is
     * Because a host runs several JDKs at once and a jar does not start under the wrong one. A
     * class file built for 25 fails on 21 with UnsupportedClassVersionError before a line of it
     * runs, and the reverse - a 21 jar on 25 - usually works but is not what anybody tested.
     * Leaving it to whichever "java" resolves first made the version an accident of the manager's
     * PATH, which is not a thing to deploy on.
     *
     * @par
     * JAVA is still "whichever java this host calls java", which is what every application
     * deployed before this said and what still runs them. JAVA21 and JAVA25 name a version and
     * get the executable configured for it - see RuntimeExecutableSetting().
     *
     * @author jensvogt47\@gmail.com
     */
    enum class Runtime {
        JAVA,
        JAVA21,
        JAVA25,
        PYTHON,
        NODEJS,
        BINARY,
        UNKNOWN
    };

    static std::map<Runtime, std::string> RuntimeNames{
            {Runtime::JAVA, "JAVA"},
            {Runtime::JAVA21, "JAVA21"},
            {Runtime::JAVA25, "JAVA25"},
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
     * Only used when an application does not spell out its own command. These are the defaults;
     * what an installation actually execs is whatever RuntimeExecutableSetting() names in the
     * configuration, falling back to the command here. Bare names rather than absolute paths, so
     * an unconfigured installation picks whichever java/python3/node it has on PATH - euclid does
     * not manage language toolchains, it only launches processes.
     *
     * @par
     * Only the executable is configurable, never the arguments: "-jar" is how a jar is started,
     * not a local preference, and an installation that could change it could produce a command
     * line that cannot work.
     *
     * @par
     * "java21" and "java25" are not names any distribution ships. That is deliberate. An
     * installation that asked for a version and configured nothing fails to exec with the name it
     * was looking for in the message, which is a better morning than a jar that silently started
     * under the wrong JDK.
     *
     * @param runtime the application's runtime.
     * @return interpreter and its leading arguments; empty for BINARY, which is its own command.
     */
    [[maybe_unused]]
    static std::vector<std::string> RuntimeCommandPrefix(const Runtime &runtime) {
        switch (runtime) {
            case Runtime::JAVA:
                return {"java", "-jar"};
            case Runtime::JAVA21:
                return {"java21", "-jar"};
            case Runtime::JAVA25:
                return {"java25", "-jar"};
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

    /**
     * @brief The configuration key naming the executable this runtime is started with.
     *
     * @par
     * How a host says where its JDKs are. A machine with three of them installed side by side has
     * no single answer to "java", and the answer differs per platform - /usr/lib/jvm on Debian,
     * /Library/Java/JavaVirtualMachines on macOS - which is why it belongs in the per-platform
     * configuration file rather than compiled in.
     *
     * @par
     * Read by the manager when it builds an application's command line, not by EAP: EAP records
     * what an application asked for, and the host it is started on decides what that means there.
     * The same application definition can therefore run on hosts whose JDKs live in different
     * places, which is the point of naming a version rather than a path.
     *
     * @param runtime the application's runtime.
     * @return the configuration key, or empty for a runtime that has no interpreter to name.
     */
    [[maybe_unused]]
    static std::string RuntimeExecutableSetting(const Runtime &runtime) {
        switch (runtime) {
            case Runtime::JAVA:
                return "euclid.modules.eap.runtimes.java";
            case Runtime::JAVA21:
                return "euclid.modules.eap.runtimes.java21";
            case Runtime::JAVA25:
                return "euclid.modules.eap.runtimes.java25";
            case Runtime::PYTHON:
                return "euclid.modules.eap.runtimes.python";
            case Runtime::NODEJS:
                return "euclid.modules.eap.runtimes.nodejs";
            case Runtime::BINARY:
            case Runtime::UNKNOWN:
                break;
        }
        return {};
    }

}// namespace Euclid::Database::Entity::EAP
