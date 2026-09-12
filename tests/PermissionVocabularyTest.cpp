// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE PermissionVocabularyTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// Euclid includes
#include <euclid/core/Permissions.h>

using Euclid::Core::Permissions;

// Core::Permissions::All() is a list of what a role can be granted. The actions it names live in
// thirteen dispatch tables in thirteen other files, and a list maintained apart from the thing it
// describes drifts the first time somebody adds an action - invisibly, because a missing permission
// is an action nobody can grant and a stale one is a grant that does nothing.
//
// So this re-derives the vocabulary from the modules themselves and compares. It is the reason the
// hand-written list is allowed to exist at all.

namespace {

    namespace fs = std::filesystem;

    // Where euclid's sources are, handed in by CMake - this test reads them rather than linking
    // them, because what it is checking is the text of the dispatch tables and not their behaviour.
    fs::path sourceRoot() {
        return fs::path(EUCLID_SOURCE_DIR);
    }

    std::string contentsOf(const fs::path &path) {
        const std::ifstream in(path);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    // Every `action == "..."` literal in a module's server. That is the one shape euclid's dispatch
    // is written in - see any modules/*/src/*Server.cpp - including the `a == "x" || a == "y"` form
    // that gives one command two names.
    std::set<std::string> actionsIn(const fs::path &server) {
        // Custom delimiter: the pattern itself contains the )" that would close a plain R"( ... )".
        static const std::regex kAction(R"re(action == "([^"]+)")re");
        const auto text = contentsOf(server);
        std::set<std::string> actions;
        for (std::sregex_iterator it(text.begin(), text.end(), kAction), end; it != end; ++it) {
            actions.insert((*it)[1].str());
        }
        return actions;
    }

    // The module directories that have a dispatching server, found rather than listed: a module
    // added to modules/ is then covered by this test without anybody remembering to add it here,
    // which is the whole point.
    std::vector<std::pair<std::string, fs::path>> moduleServers() {
        std::vector<std::pair<std::string, fs::path>> servers;
        for (const auto &entry: fs::directory_iterator(sourceRoot() / "modules")) {
            if (!entry.is_directory()) continue;
            const auto module = entry.path().filename().string();
            for (const auto &file: fs::directory_iterator(entry.path() / "src")) {
                if (file.path().filename().string().ends_with("Server.cpp")) {
                    servers.emplace_back(module, file.path());
                }
            }
        }
        std::ranges::sort(servers);
        return servers;
    }

    bool isUnbindable(const std::string &module) {
        return std::ranges::contains(Permissions::UnbindableModules(), module);
    }

    // What Permissions::All() would have to be for the modules as they stand today.
    std::set<std::string> vocabularyFromSources() {
        std::set<std::string> permissions;
        for (const auto &[module, server]: moduleServers()) {
            if (isUnbindable(module)) continue;
            for (const auto &action: actionsIn(server)) {
                permissions.insert(module + ":" + action);
            }
        }
        return permissions;
    }

    std::string joined(const std::set<std::string> &values) {
        std::string text;
        for (const auto &value: values) text += "\n    " + value;
        return text.empty() ? " (none)" : text;
    }

}// namespace

BOOST_AUTO_TEST_CASE(TheSourcesAreWhereThisThinksTheyAre) {

    // A wrong path would make every comparison below pass against an empty set, which is the one
    // way this test could be silently useless.
    BOOST_REQUIRE(fs::is_directory(sourceRoot() / "modules"));
    BOOST_REQUIRE(!moduleServers().empty());
    BOOST_TEST(!vocabularyFromSources().empty());
}

BOOST_AUTO_TEST_CASE(EveryDispatchedActionHasAPermission) {

    const auto expected = vocabularyFromSources();
    const std::set<std::string> actual(Permissions::All().begin(), Permissions::All().end());

    std::set<std::string> missing;
    std::ranges::set_difference(expected, actual, std::inserter(missing, missing.end()));

    BOOST_TEST(missing.empty(),
               "a module dispatches these actions and Core::Permissions::All() does not name them, so "
               "nobody can be granted them - add to core/src/Permissions.cpp:" + joined(missing));
}

BOOST_AUTO_TEST_CASE(EveryPermissionNamesADispatchedAction) {

    const auto expected = vocabularyFromSources();
    const std::set<std::string> actual(Permissions::All().begin(), Permissions::All().end());

    std::set<std::string> stale;
    std::ranges::set_difference(actual, expected, std::inserter(stale, stale.end()));

    BOOST_TEST(stale.empty(),
               "Core::Permissions::All() names these and no module dispatches them, so granting one "
               "grants nothing - remove from core/src/Permissions.cpp:" + joined(stale));
}

BOOST_AUTO_TEST_CASE(TheVocabularyIsSortedAndUnique) {

    // Exists() binary-searches it, which is wrong on an unsorted vector and would answer false for
    // a permission that is there.
    BOOST_TEST(std::ranges::is_sorted(Permissions::All()));

    const bool hasDuplicates = std::ranges::adjacent_find(Permissions::All()) != Permissions::All().end();
    BOOST_TEST(!hasDuplicates);
}

BOOST_AUTO_TEST_CASE(TheUnbindableModulesAreNotInTheVocabulary) {

    // Not "have no permissions granted anywhere" but "have none to grant": emm manages every
    // module's processes, emd is the document store itself.
    BOOST_REQUIRE(!Permissions::UnbindableModules().empty());

    for (const auto &module: Permissions::UnbindableModules()) {
        const auto prefix = module + ":";
        for (const auto &permission: Permissions::All()) {
            BOOST_TEST(!permission.starts_with(prefix), module + " must not be grantable, and " + permission + " is");
        }
        BOOST_TEST(!Permissions::IsBindable(module));
        BOOST_TEST(!std::ranges::contains(Permissions::Modules(), module));
    }
}

BOOST_AUTO_TEST_CASE(TheUnbindableModulesReallyHaveActionsToExclude) {

    // Otherwise the test above passes because those modules dispatch nothing, rather than because
    // they were deliberately left out - and a module renamed away would go unnoticed.
    const auto servers = moduleServers();

    for (const auto &module: Permissions::UnbindableModules()) {
        const auto server = std::ranges::find_if(servers, [&](const auto &pair) { return pair.first == module; });
        BOOST_REQUIRE_MESSAGE(server != servers.end(), "no server found for unbindable module " + module);
        BOOST_TEST(!actionsIn(server->second).empty(), module + " dispatches nothing, so excluding it means nothing");
    }
}

// The hole this closes: the vocabulary is derived by reading `action == "..."` literals, so a module
// that dispatched on a named constant instead - as emd does, with P::kFindOne and the rest - would
// contribute actions this cannot see, and they would be missing from All() with nothing to say so.
// emd is excluded wholesale, so it does not matter there; anywhere else it would.
BOOST_AUTO_TEST_CASE(EveryBindableModuleDispatchesOnStringLiterals) {

    // `action == something` where something is not a quote: a constant, and therefore invisible
    // to the derivation above.
    static const std::regex kNonLiteral(R"re(action == [^"\s])re");

    for (const auto &[module, server]: moduleServers()) {
        if (isUnbindable(module)) continue;

        const auto text = contentsOf(server);
        const bool dispatchesOnAConstant = std::regex_search(text, kNonLiteral);
        BOOST_TEST(!dispatchesOnAConstant,
                   module + " compares x-euclid-action against something other than a string literal, so this test "
                            "cannot see all of its actions - either spell them out or exclude the module deliberately");
    }
}

BOOST_AUTO_TEST_CASE(ModulesAreTheModulesThatHaveActions) {

    BOOST_TEST(!Permissions::Modules().empty());
    for (const auto &module: Permissions::Modules()) {
        BOOST_TEST(Permissions::IsBindable(module));
    }
    BOOST_TEST(!Permissions::IsBindable("nonexistent"));
}

// ── Matching ────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(AnExactGrantMatchesItself) {

    BOOST_TEST(Permissions::Matches("ens:publish-message", "ens:publish-message"));
    BOOST_TEST(!Permissions::Matches("ens:publish-message", "ens:delete-topic"));
    BOOST_TEST(!Permissions::Matches("ens:publish-message", "eqs:send-message"));
}

BOOST_AUTO_TEST_CASE(AModuleWildcardMatchesThatModuleOnly) {

    BOOST_TEST(Permissions::Matches("ens:*", "ens:publish-message"));
    BOOST_TEST(Permissions::Matches("ens:*", "ens:delete-topic"));
    BOOST_TEST(!Permissions::Matches("ens:*", "eqs:send-message"));
}

BOOST_AUTO_TEST_CASE(EverythingMatchesEveryPermission) {

    for (const auto &permission: Permissions::All()) {
        BOOST_TEST(Permissions::Matches(Permissions::Everything, permission), permission + " is not covered by *:*");
    }
}

// The point of refusing an unknown required permission: emm cannot be reached even by *:*, because
// no emm action is in the vocabulary to be required.
BOOST_AUTO_TEST_CASE(NothingReachesAnUnbindableModuleNotEvenEverything) {

    BOOST_TEST(!Permissions::Matches(Permissions::Everything, "emm:import"));
    BOOST_TEST(!Permissions::Matches(Permissions::Everything, "emd:replace-one"));
    BOOST_TEST(!Permissions::Matches(Permissions::Everything, "emm:start-module"));
    BOOST_TEST(!Permissions::Matches("emm:*", "emm:import"));
}

// A handler that mistypes its own action fails closed rather than matching a wildcard.
BOOST_AUTO_TEST_CASE(AnUnknownRequiredPermissionIsRefused) {

    BOOST_TEST(!Permissions::Matches(Permissions::Everything, "ens:publish-mesage"));
    BOOST_TEST(!Permissions::Matches("ens:*", "ens:not-an-action"));
    BOOST_TEST(!Permissions::Matches(Permissions::Everything, ""));
    BOOST_TEST(!Permissions::Matches(Permissions::Everything, "ens"));
}

BOOST_AUTO_TEST_CASE(AWildcardIsNotAcceptedOnTheRequiredSide) {

    // A request asks for one action. Requiring "ens:*" would be asking whether the caller may do
    // everything ENS offers, which no handler means.
    BOOST_TEST(!Permissions::Matches(Permissions::Everything, "ens:*"));
    BOOST_TEST(!Permissions::Matches("ens:*", "ens:*"));
}

BOOST_AUTO_TEST_CASE(OnlyTheModulePositionTakesAWildcard) {

    // "every module's publish-message" is not a grant this understands.
    BOOST_TEST(!Permissions::Matches("*:publish-message", "ens:publish-message"));
    BOOST_TEST(!Permissions::Matches("*", "ens:publish-message"));
    BOOST_TEST(!Permissions::Matches("ens:publish-*", "ens:publish-message"));
}

BOOST_AUTO_TEST_CASE(ExistsAnswersForTheVocabularyAlone) {

    BOOST_TEST(Permissions::Exists("ens:publish-message"));
    BOOST_TEST(Permissions::Exists("eqs:receive-messages"));
    BOOST_TEST(Permissions::Exists("esm:put-object"));

    BOOST_TEST(!Permissions::Exists("ens:*"));
    BOOST_TEST(!Permissions::Exists(Permissions::Everything));
    BOOST_TEST(!Permissions::Exists("emm:import"));
    BOOST_TEST(!Permissions::Exists("emd:find-one"));
    BOOST_TEST(!Permissions::Exists("ens:no-such-action"));
}

BOOST_AUTO_TEST_CASE(OfNamesAnActionTheWayTheWireDoes) {

    // The x-euclid-target and x-euclid-action headers, joined. The three the role concept opens
    // with, so a typo in either would show here.
    BOOST_TEST(Permissions::Of("ens", "create-topic") == "ens:create-topic");
    BOOST_TEST(Permissions::Exists(Permissions::Of("ens", "create-topic")));
    BOOST_TEST(Permissions::Exists(Permissions::Of("ens", "publish-message")));
    BOOST_TEST(Permissions::Exists(Permissions::Of("ens", "subscribe")));
}
