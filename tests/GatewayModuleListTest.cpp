// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE GatewayModuleListTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

// The gateway decides whether a request is for a euclid module by looking its x-euclid-target up in
// a hand-written set - detectEuclidService() in main/src/GatewayServer.cpp. A module missing from
// that set is built, configured, started, registered and reachable by nothing: every request for it
// falls past the dispatch and is answered with a bare 404 that says only "not found", which is the
// least informative failure in the system and points at the client rather than at the list.
//
// That is exactly what happened when EAD was added. The module was wired into the manager, the
// permissions, the roles, the CLI and four config files - and not into this one set, so the first
// command anybody ran returned 404.
//
// Derived from the modules/ directory rather than from a second list, the same way
// PermissionVocabularyTest derives the vocabulary: a list checked against another list somebody
// maintains is two lists to get wrong.

namespace {

    fs::path sourceRoot() {
        return fs::path(EUCLID_SOURCE_DIR);
    }

    std::string contentsOf(const fs::path &path) {
        const std::ifstream in(path);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    // The module directories that have a dispatching server. Found rather than listed, so a module
    // added to modules/ is covered by this test without anybody remembering it here.
    std::set<std::string> modulesOnDisk() {
        std::set<std::string> modules;
        for (const auto &entry: fs::directory_iterator(sourceRoot() / "modules")) {
            if (!entry.is_directory()) continue;
            for (const auto &file: fs::directory_iterator(entry.path() / "src")) {
                if (file.path().filename().string().ends_with("Server.cpp")) {
                    modules.insert(entry.path().filename().string());
                }
            }
        }
        return modules;
    }

    // The contents of the kModules set literal, read out of the gateway's source. The text of the
    // thing that decides, not a copy of it.
    std::set<std::string> gatewayModules() {

        const auto text = contentsOf(sourceRoot() / "main" / "src" / "GatewayServer.cpp");

        static const std::regex kSet(R"re(kModules\{([^}]*)\})re");
        std::smatch match;
        if (!std::regex_search(text, match, kSet)) return {};

        static const std::regex kEntry(R"re("([a-z]+)")re");
        const auto body = match[1].str();

        std::set<std::string> modules;
        for (std::sregex_iterator it(body.begin(), body.end(), kEntry), end; it != end; ++it) {
            modules.insert((*it)[1].str());
        }
        return modules;
    }

    // The document store. It is addressed by the other modules over their own sockets and has no
    // business being reachable from outside, so it is the one module that must not be in the set.
    bool isInternalOnly(const std::string &module) {
        return module == "emd";
    }

}// namespace

BOOST_AUTO_TEST_SUITE(GatewayModuleListTest)

BOOST_AUTO_TEST_CASE(TheGatewayListWasFoundAtAll) {

    // Guards every other case here: a renamed set or a reformatted literal would make the regex
    // find nothing, and "nothing is missing from an empty set" would pass quietly.
    BOOST_TEST(!gatewayModules().empty());
    BOOST_TEST(modulesOnDisk().size() > 5U);
}

BOOST_AUTO_TEST_CASE(EveryModuleIsReachableThroughTheGateway) {

    const auto onDisk = modulesOnDisk();
    const auto routed = gatewayModules();

    for (const auto &module: onDisk) {
        if (isInternalOnly(module)) continue;
        BOOST_TEST_CONTEXT("module " << module) {
            const bool reachable = routed.contains(module);
            BOOST_TEST(reachable, module + " has a dispatching server but is not in the gateway's "
                                           "kModules, so every request for it is answered 404");
        }
    }
}

BOOST_AUTO_TEST_CASE(TheGatewayRoutesNothingThatIsNotAModule) {

    // The other direction: a name left in the set after its module was removed or renamed sends
    // requests to a pool that does not exist, which fails later and less clearly.
    const auto onDisk = modulesOnDisk();

    for (const auto &module: gatewayModules()) {
        BOOST_TEST_CONTEXT("module " << module) {
            const bool exists = onDisk.contains(module);
            BOOST_TEST(exists, module + " is in the gateway's kModules but no such module exists");
        }
    }
}

BOOST_AUTO_TEST_CASE(TheDocumentStoreIsNotReachableFromOutside) {

    // emd holds every other module's data and authenticates nobody: it is reached over a socket by
    // processes that are already inside. Routing it from the gateway would put the store itself on
    // the public surface.
    const bool routed = gatewayModules().contains("emd");
    BOOST_TEST(!routed);
}

BOOST_AUTO_TEST_SUITE_END()
