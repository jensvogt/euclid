// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE WorkerCommandTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <map>
#include <string>
#include <vector>

// Euclid includes
#include <WorkerCommand.h>
#include <euclid/database/entity/eap/Runtime.h>

using Euclid::Worker::CommandLine;
using Euclid::Database::Entity::EAP::Runtime;
using Euclid::Database::Entity::EAP::RuntimeCommandPrefix;
using Euclid::Database::Entity::EAP::RuntimeToString;

// What an application is started with on a node. The worker restates the manager's rule rather
// than sharing it, because it does not link eucliddb - so the first case here is the one that
// keeps the two in step: an unconfigured node starts every runtime exactly as an unconfigured
// manager does.

namespace {

    const std::string kArtifact = "/var/lib/euclid-wrk/applications/demo/demo.jar";

    // A node with nothing configured: every key answers its default.
    std::string unconfigured(const std::string &, const std::string &fallback) { return fallback; }

    auto configured(std::map<std::string, std::string> settings) {
        return [settings = std::move(settings)](const std::string &key, const std::string &fallback) {
            const auto found = settings.find(key);
            return found != settings.end() ? found->second : fallback;
        };
    }

}// namespace

BOOST_AUTO_TEST_CASE(AnUnconfiguredNodeStartsEveryRuntimeAsTheManagerDoes) {

    for (const auto runtime: {Runtime::JAVA, Runtime::JAVA21, Runtime::JAVA25, Runtime::PYTHON, Runtime::NODEJS, Runtime::BINARY}) {
        auto expected = RuntimeCommandPrefix(runtime);
        expected.push_back(kArtifact);
        BOOST_TEST_CONTEXT("runtime " << RuntimeToString(runtime)) {
            BOOST_TEST(CommandLine(RuntimeToString(runtime), "", kArtifact, {}, unconfigured) == expected);
        }
    }
}

BOOST_AUTO_TEST_CASE(PythonAndNodeAreNotStartedAsJars) {

    // The failure this replaced: everything that was not BINARY was handed to java -jar.
    BOOST_TEST(CommandLine("PYTHON", "", "app.py", {}, unconfigured) == (std::vector<std::string>{"python3", "app.py"}));
    BOOST_TEST(CommandLine("NODEJS", "", "app.js", {}, unconfigured) == (std::vector<std::string>{"node", "app.js"}));
}

BOOST_AUTO_TEST_CASE(EachInterpreterIsWhereTheNodeSaysItIs) {

    const auto settings = configured({{"euclid.worker.runtimes.java21", "/usr/lib/jvm/java-21/bin/java"},
                                      {"euclid.worker.runtimes.python", "/opt/python/bin/python3"},
                                      {"euclid.worker.runtimes.nodejs", "/opt/node/bin/node"}});

    BOOST_TEST(CommandLine("JAVA21", "", "a.jar", {}, settings) == (std::vector<std::string>{"/usr/lib/jvm/java-21/bin/java", "-jar", "a.jar"}));
    BOOST_TEST(CommandLine("PYTHON", "", "a.py", {}, settings) == (std::vector<std::string>{"/opt/python/bin/python3", "a.py"}));
    BOOST_TEST(CommandLine("NODEJS", "", "a.js", {}, settings) == (std::vector<std::string>{"/opt/node/bin/node", "a.js"}));
    // Not configured, so the name nothing ships - which fails to exec with the version in it.
    BOOST_TEST(CommandLine("JAVA25", "", "a.jar", {}, settings).front() == "java25");
}

BOOST_AUTO_TEST_CASE(TheOlderJavaSettingStillCountsForJava) {

    // euclid.worker.java is what every worker configured before the per-runtime keys says.
    BOOST_TEST(CommandLine("JAVA", "", "a.jar", {}, configured({{"euclid.worker.java", "/opt/jdk/bin/java"}})).front() == "/opt/jdk/bin/java");

    // The newer key wins where both are set.
    BOOST_TEST(CommandLine("JAVA", "", "a.jar", {},
                           configured({{"euclid.worker.java", "/opt/old/java"}, {"euclid.worker.runtimes.java", "/opt/new/java"}}))
                       .front() == "/opt/new/java");

    // And it is JAVA's alone: a versioned runtime is not quietly started under it.
    BOOST_TEST(CommandLine("JAVA21", "", "a.jar", {}, configured({{"euclid.worker.java", "/opt/jdk/bin/java"}})).front() == "java21");
}

BOOST_AUTO_TEST_CASE(AnApplicationsOwnCommandOverridesItsRuntime) {

    BOOST_TEST(CommandLine("JAVA", "/opt/wrapper.sh", "a.jar", {"--port", "8080"}, unconfigured)
               == (std::vector<std::string>{"/opt/wrapper.sh", "a.jar", "--port", "8080"}));
}

BOOST_AUTO_TEST_CASE(ArgumentsComeLast) {

    BOOST_TEST(CommandLine("BINARY", "", "./server", {"--verbose"}, unconfigured) == (std::vector<std::string>{"./server", "--verbose"}));
    BOOST_TEST(CommandLine("JAVA", "", "a.jar", {"-x"}, unconfigured) == (std::vector<std::string>{"java", "-jar", "a.jar", "-x"}));
}

BOOST_AUTO_TEST_CASE(AnUnknownRuntimeIsTheArtifactItself) {

    BOOST_TEST(CommandLine("", "", "./thing", {}, unconfigured) == (std::vector<std::string>{"./thing"}));
}
