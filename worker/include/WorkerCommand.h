// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <cctype>
#include <functional>
#include <string>
#include <vector>

namespace Euclid::Worker {

    /**
     * @brief The command line an assigned instance is started with: the executable first, then its
     * arguments.
     *
     * @par
     * The same rule the manager applies to the applications it runs itself - see
     * ServiceController's spawn and Database::Entity::EAP::RuntimeCommandPrefix() - restated here
     * rather than shared, because a worker deliberately does not link eucliddb. An application has
     * to start the same way whichever host it lands on, so the two are kept in step by the tests
     * rather than by a common header:
     *
     *   - a command the application spells out is the executable, with the artifact as its first
     *     argument, and overrides everything else;
     *   - otherwise the runtime names an interpreter: java -jar for the three Java runtimes,
     *     python3 for PYTHON, node for NODEJS;
     *   - BINARY, and anything not recognised, is the artifact itself.
     *
     * @par
     * Only the interpreter is configurable, never its arguments, for the reason the manager gives:
     * "-jar" is how a jar is started, not a preference. Where this host keeps each interpreter is
     * euclid.worker.runtimes.<java|java21|java25|python|nodejs>; euclid.worker.java is still read
     * for JAVA, since it is what every worker configured before the others existed says.
     *
     * @par
     * A pure function of its inputs, the configuration read through @p setting, so every case is
     * assertable without a fork - the same reason Reconciler::Decide() is one.
     *
     * @param runtime the application's runtime, as the master names it ("JAVA21", "BINARY", ...).
     * @param command the application's own command; empty for the ordinary case.
     * @param artifact where the fetched artifact is on this host.
     * @param arguments the application's arguments, appended after everything else.
     * @param setting reads a configuration key, answering the given default when it is not set.
     * @return the command line; never empty.
     */
    [[nodiscard]] inline std::vector<std::string> CommandLine(
            const std::string &runtime, const std::string &command, const std::string &artifact,
            const std::vector<std::string> &arguments,
            const std::function<std::string(const std::string &key, const std::string &fallback)> &setting) {

        std::vector<std::string> line;

        if (!command.empty()) {
            line = {command, artifact};
        } else if (runtime == "JAVA" || runtime == "JAVA21" || runtime == "JAVA25") {
            // "java21" and "java25" are not names any distribution ships, which is the manager's
            // choice too: a node asked for a version it was never told the location of fails to exec
            // with that name in the message, rather than starting the jar under some other JDK.
            const auto fallback = runtime == "JAVA" ? setting("euclid.worker.java", "java")
                                  : runtime == "JAVA21" ? std::string("java21")
                                                        : std::string("java25");
            std::string key = runtime;
            for (auto &c: key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            line = {setting("euclid.worker.runtimes." + key, fallback), "-jar", artifact};
        } else if (runtime == "PYTHON") {
            line = {setting("euclid.worker.runtimes.python", "python3"), artifact};
        } else if (runtime == "NODEJS") {
            line = {setting("euclid.worker.runtimes.nodejs", "node"), artifact};
        } else {
            line = {artifact};
        }

        line.insert(line.end(), arguments.begin(), arguments.end());
        return line;
    }

}// namespace Euclid::Worker
