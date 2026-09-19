// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE CliCompletionTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <string>
#include <vector>

// Euclid includes
#include <euclid/cli/help/CliCompletion.h>

using Euclid::CLI::Completion::Candidates;
using Euclid::CLI::Completion::Request;
using Euclid::CLI::Completion::Split;

// What may follow what has been typed, decided without a binary that knows thirteen modules -
// the tables are arguments, so these are about the rules rather than about the CLI's contents.
//
// The one rule worth stating out loud: a completer invoked through `complete -C` does its own
// filtering. The shell offers whatever it is handed, so a candidate list that ignored the word
// under the cursor would offer every action of every module on every keystroke.

namespace {

    const std::vector<std::string> kModules{"eam", "eap", "eqs", "esm"};
    const std::vector<std::string> kGlobals{"--endpoint", "--pretty", "--version"};

    std::vector<std::string> ActionsOf(const std::string &module) {
        if (module == "eap") return {"create-application", "restart-application", "start-application"};
        if (module == "eqs") return {"purge-queue", "send-message"};
        return {};
    }

    std::vector<std::string> OptionsOf(const std::string &module, const std::string &action) {
        if (module == "eap" && action == "restart-application") return {"--application-id"};
        if (module == "eqs" && action == "send-message") return {"--queue", "--body", "--priority"};
        return {};
    }

    std::vector<std::string> CandidatesFor(const std::string &line) {
        return Candidates(Split(line, line.size()), kModules, ActionsOf, OptionsOf, kGlobals);
    }

}// namespace

BOOST_AUTO_TEST_CASE(TheFirstWordIsAModule) {
    const auto candidates = CandidatesFor("euclid-cli ");
    BOOST_TEST_REQUIRE(candidates.size() == 4U);
    BOOST_TEST(candidates[0] == "eam");
    BOOST_TEST(candidates[3] == "esm");
}

BOOST_AUTO_TEST_CASE(APartialModuleIsNarrowedToWhatItCouldBe) {
    const auto candidates = CandidatesFor("euclid-cli ea");
    BOOST_TEST_REQUIRE(candidates.size() == 2U);
    BOOST_TEST(candidates[0] == "eam");
    BOOST_TEST(candidates[1] == "eap");
}

BOOST_AUTO_TEST_CASE(AModuleIsFollowedByItsActions) {
    const auto candidates = CandidatesFor("euclid-cli eap ");

    // "help" is offered everywhere and belongs to no module's table, so it is added rather than
    // listed - a module that put it in its own actions would be answering for the framework.
    BOOST_TEST(std::ranges::contains(candidates, std::string("help")));
    BOOST_TEST(std::ranges::contains(candidates, std::string("restart-application")));
    BOOST_TEST(!std::ranges::contains(candidates, std::string("purge-queue")));
}

BOOST_AUTO_TEST_CASE(APartialActionIsNarrowed) {
    const auto candidates = CandidatesFor("euclid-cli eap st");
    BOOST_TEST_REQUIRE(candidates.size() == 1U);
    BOOST_TEST(candidates[0] == "start-application");
}

BOOST_AUTO_TEST_CASE(AWordThatIsNotAModuleCompletesToNothing) {
    BOOST_TEST(CandidatesFor("euclid-cli nonsense ").empty());
}

BOOST_AUTO_TEST_CASE(AnOptionIsCompletedFromTheActionsOwnOptions) {
    const auto candidates = CandidatesFor("euclid-cli eqs send-message --");
    BOOST_TEST_REQUIRE(candidates.size() == 3U);
    BOOST_TEST(candidates[0] == "--body");
    BOOST_TEST(candidates[1] == "--priority");
    BOOST_TEST(candidates[2] == "--queue");
}

BOOST_AUTO_TEST_CASE(APartialOptionIsNarrowed) {
    const auto candidates = CandidatesFor("euclid-cli eqs send-message --p");
    BOOST_TEST_REQUIRE(candidates.size() == 1U);
    BOOST_TEST(candidates[0] == "--priority");
}

BOOST_AUTO_TEST_CASE(AValueIsLeftToTheShell) {
    // Nothing is offered where a value goes, rather than a guess: the shell falls back to file
    // names, which is the right answer for the options that take one.
    BOOST_TEST(CandidatesFor("euclid-cli eqs send-message --queue ").empty());
}

BOOST_AUTO_TEST_CASE(GlobalOptionsComeBeforeTheModule) {
    const auto candidates = CandidatesFor("euclid-cli --p");
    BOOST_TEST_REQUIRE(candidates.size() == 1U);
    BOOST_TEST(candidates[0] == "--pretty");
}

BOOST_AUTO_TEST_CASE(AGlobalOptionAndItsValueDoNotCountAsTheModule) {
    // "--endpoint https://host eq" is still looking for a module: the value belongs to the option
    // in front of it, and counting it as the module would offer actions of a module nobody named.
    const auto candidates = CandidatesFor("euclid-cli --endpoint https://host eq");
    BOOST_TEST_REQUIRE(candidates.size() == 1U);
    BOOST_TEST(candidates[0] == "eqs");
}

BOOST_AUTO_TEST_CASE(AnAttachedOptionValueTakesNoWordOfItsOwn) {
    const auto candidates = CandidatesFor("euclid-cli --endpoint=https://host ea");
    BOOST_TEST_REQUIRE(candidates.size() == 2U);
    BOOST_TEST(candidates[0] == "eam");
}

BOOST_AUTO_TEST_CASE(TheCursorDecidesWhatIsBeingCompletedNotTheEndOfTheLine) {
    // The cursor sits after "eq", with "send-message" still to its right. What is being completed
    // is the module, and the rest of the line is not evidence about it.
    const std::string line = "euclid-cli eq send-message";
    const auto candidates = Candidates(Split(line, 13), kModules, ActionsOf, OptionsOf, kGlobals);
    BOOST_TEST_REQUIRE(candidates.size() == 1U);
    BOOST_TEST(candidates[0] == "eqs");
}

BOOST_AUTO_TEST_CASE(AQuotedValueIsOneWord) {
    // A quoted body with spaces in it is a single word, so what follows is still an option rather
    // than whatever the last space happened to separate.
    const auto candidates = CandidatesFor("euclid-cli eqs send-message --body \"hello there\" --");
    BOOST_TEST_REQUIRE(candidates.size() == 3U);
    BOOST_TEST(candidates[0] == "--body");
}

BOOST_AUTO_TEST_CASE(AnEmptyLineOffersTheModules) {
    BOOST_TEST(CandidatesFor("euclid-cli").size() == 4U);
}
