// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE AuthorizationGapTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <algorithm>
#include <string>

// Euclid includes
#include <euclid/database/Database.h>
#include <euclid/database/repository/eam/MongoEamRepository.h>

using Euclid::Database::MongoEamRepository;
using Euclid::Database::Entity::EAM::AuthorizationGap;

// Shadow mode's whole value is the list it leaves behind, and the list is only readable if it stays
// short: a week of shadow mode on a busy installation is millions of refusals and a handful of
// distinct gaps. Recording per request rather than per gap would produce something nobody reads,
// and "nobody read it" is indistinguishable from "there was nothing to read" right up to the moment
// enforce is switched on.

namespace {

    MongoEamRepository freshRepository() {
        Euclid::Database::Database::instance().initializeMemory();
        return MongoEamRepository{};
    }

    AuthorizationGap gapOf(const std::string &userId, const std::string &target, const std::string &action,
                           const std::string &nameSpace = "production", const std::string &reason = "no grant applies") {
        const auto now = std::chrono::system_clock::now();
        return {.userId = userId, .accountId = "000000000000", .nameSpace = nameSpace, .target = target,
                .action = action, .reason = reason, .firstSeen = now, .lastSeen = now};
    }

}// namespace

BOOST_AUTO_TEST_CASE(AGapRoundTrips) {

    auto repo = freshRepository();
    repo.recordAuthorizationGap(gapOf("jens", "ens", "publish-message"));

    const auto gaps = repo.listAuthorizationGaps("", 0, 0);

    BOOST_REQUIRE(gaps.size() == 1U);
    BOOST_TEST(gaps.front().userId == "jens");
    BOOST_TEST(gaps.front().target == "ens");
    BOOST_TEST(gaps.front().action == "publish-message");
    BOOST_TEST(gaps.front().nameSpace == "production");
    BOOST_TEST(gaps.front().reason == "no grant applies");
    BOOST_TEST(gaps.front().count == 1L);
}

// The point of the whole design: the same refusal a thousand times is one row with a count, not a
// thousand rows.
BOOST_AUTO_TEST_CASE(TheSameGapIsCountedNotRepeated) {

    auto repo = freshRepository();
    for (int i = 0; i < 5; ++i) repo.recordAuthorizationGap(gapOf("jens", "ens", "publish-message"));

    const auto gaps = repo.listAuthorizationGaps("", 0, 0);

    BOOST_REQUIRE(gaps.size() == 1U);
    BOOST_TEST(gaps.front().count == 5L);
    BOOST_TEST(repo.countAuthorizationGaps() == 1L);
}

BOOST_AUTO_TEST_CASE(GapsAreDistinctByEveryPartOfTheQuestion) {

    auto repo = freshRepository();
    repo.recordAuthorizationGap(gapOf("jens", "ens", "publish-message"));
    repo.recordAuthorizationGap(gapOf("jill", "ens", "publish-message"));           // other user
    repo.recordAuthorizationGap(gapOf("jens", "eqs", "publish-message"));           // other module
    repo.recordAuthorizationGap(gapOf("jens", "ens", "delete-topic"));              // other action
    repo.recordAuthorizationGap(gapOf("jens", "ens", "publish-message", "staging"));// other namespace

    BOOST_TEST(repo.countAuthorizationGaps() == 5L);
}

// Most-seen first, because the gap refused ten thousand times is the grant to write before the one
// refused once.
BOOST_AUTO_TEST_CASE(GapsComeBackMostSeenFirst) {

    auto repo = freshRepository();
    repo.recordAuthorizationGap(gapOf("jens", "ens", "delete-topic"));
    for (int i = 0; i < 10; ++i) repo.recordAuthorizationGap(gapOf("jens", "ens", "publish-message"));
    for (int i = 0; i < 3; ++i) repo.recordAuthorizationGap(gapOf("jens", "eqs", "send-message"));

    const auto gaps = repo.listAuthorizationGaps("", 0, 0);

    BOOST_REQUIRE(gaps.size() == 3U);
    BOOST_TEST(gaps[0].action == "publish-message");
    BOOST_TEST(gaps[0].count == 10L);
    BOOST_TEST(gaps[1].action == "send-message");
    BOOST_TEST(gaps[2].action == "delete-topic");
}

BOOST_AUTO_TEST_CASE(GapsCanBeNarrowedByModuleOrAction) {

    auto repo = freshRepository();
    repo.recordAuthorizationGap(gapOf("jens", "ens", "publish-message"));
    repo.recordAuthorizationGap(gapOf("jens", "ens", "delete-topic"));
    repo.recordAuthorizationGap(gapOf("jens", "eqs", "send-message"));

    BOOST_TEST(repo.listAuthorizationGaps("ens:", 0, 0).size() == 2U);
    BOOST_TEST(repo.listAuthorizationGaps("ens:publish", 0, 0).size() == 1U);
    BOOST_TEST(repo.listAuthorizationGaps("esm:", 0, 0).empty());
}

// The reason is kept fresh while the count accumulates: a gap whose cause changed - a grant was
// written that applies but does not cover this action - should read as it is now, not as it was.
BOOST_AUTO_TEST_CASE(TheLatestReasonWins) {

    auto repo = freshRepository();
    repo.recordAuthorizationGap(gapOf("jens", "ens", "publish-message", "production", "no grant applies"));
    repo.recordAuthorizationGap(gapOf("jens", "ens", "publish-message", "production", "no role granted here holds it"));

    const auto gaps = repo.listAuthorizationGaps("", 0, 0);

    BOOST_REQUIRE(gaps.size() == 1U);
    BOOST_TEST(gaps.front().count == 2L);
    BOOST_TEST(gaps.front().reason == "no role granted here holds it");
}

// What an operator does after writing a round of grants, so the next round shows what is still
// missing rather than what used to be.
BOOST_AUTO_TEST_CASE(ClearingForgetsEverything) {

    auto repo = freshRepository();
    repo.recordAuthorizationGap(gapOf("jens", "ens", "publish-message"));
    repo.recordAuthorizationGap(gapOf("jill", "eqs", "send-message"));

    repo.clearAuthorizationGaps();

    BOOST_TEST(repo.listAuthorizationGaps("", 0, 0).empty());
    BOOST_TEST(repo.countAuthorizationGaps() == 0L);
}

BOOST_AUTO_TEST_CASE(GapsArePaged) {

    auto repo = freshRepository();
    for (int i = 0; i < 7; ++i) repo.recordAuthorizationGap(gapOf("jens", "ens", "action-" + std::to_string(i)));

    BOOST_TEST(repo.listAuthorizationGaps("", 3, 0).size() == 3U);
    BOOST_TEST(repo.listAuthorizationGaps("", 3, 2).size() == 1U);
    BOOST_TEST(repo.countAuthorizationGaps() == 7L);
}

// An installation that never turned shadow mode on has no gaps, which must read as "nothing has
// been watching" rather than "nothing is missing". The store cannot tell those apart - which is
// why the action answers with the current mode alongside the list.
BOOST_AUTO_TEST_CASE(AnEmptyStoreIsJustEmpty) {

    auto repo = freshRepository();

    BOOST_TEST(repo.listAuthorizationGaps("", 0, 0).empty());
    BOOST_TEST(repo.countAuthorizationGaps() == 0L);
}
