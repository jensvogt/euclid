// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE RoleRepositoryTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <algorithm>
#include <string>

// Euclid includes
#include <euclid/database/Database.h>
#include <euclid/database/repository/eam/MongoEamRepository.h>

using Euclid::Database::MongoEamRepository;
using Euclid::Database::Entity::EAM::Grant;
using Euclid::Database::Entity::EAM::Role;

// Roles and grants are the two collections authorization reads on every request, and the scoping
// rules live as much in the queries as in Authorization::Allows() - a findRoleByName that ignored
// the account would let one account's role answer for another's, and Allows() would never know.

namespace {

    constexpr auto kAccount = "000000000000";
    constexpr auto kOtherAccount = "111111111111";
    constexpr auto kUser = "ern:eam:eu-central-1:000000000000:user/order-service";
    constexpr auto kGroup = "ern:eam:eu-central-1:000000000000:usergroup/services";

    MongoEamRepository freshRepository() {
        Euclid::Database::Database::instance().initializeMemory();
        return MongoEamRepository{};
    }

    Role roleOf(const std::string &name, const std::vector<std::string> &permissions, const std::string &accountId = kAccount) {
        return {.name = name, .ern = "ern:eam:eu-central-1:" + accountId + "::role/" + name, .accountId = accountId,
                .region = "eu-central-1", .description = "a role", .permissions = permissions,
                .created = std::chrono::system_clock::now(), .modified = std::chrono::system_clock::now()};
    }

    Grant grantOf(const std::string &role, const std::string &principal, const std::string &accountId = kAccount,
                  const std::vector<std::string> &namespaces = {"*"}, const std::vector<std::string> &resources = {"*"}) {
        return {.role = role, .principal = principal, .accountId = accountId, .namespaces = namespaces,
                .resources = resources, .granted = std::chrono::system_clock::now(), .grantedBy = "jens"};
    }

    std::vector<std::string> namesOf(const std::vector<Role> &roles) {
        std::vector<std::string> names;
        for (const auto &role: roles) names.push_back(role.name);
        return names;
    }

}// namespace

// ── Roles ───────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(ARoleRoundTrips) {

    auto repo = freshRepository();
    auto role = roleOf("topic-publisher", {"ens:publish-message", "ens:get-topic-ern"});

    std::ignore = repo.upsertRole(role);
    const auto found = repo.findRoleByName(kAccount, "topic-publisher");

    BOOST_REQUIRE(found.has_value());
    BOOST_TEST(found->name == "topic-publisher");
    BOOST_TEST(found->accountId == kAccount);
    BOOST_TEST(found->description == "a role");
    BOOST_REQUIRE(found->permissions.size() == 2U);
    BOOST_TEST(std::ranges::contains(found->permissions, std::string("ens:publish-message")));
    BOOST_TEST(std::ranges::contains(found->permissions, std::string("ens:get-topic-ern")));
}

// A role name is unique within an account, not across the installation. Matching on the name alone
// would have one account's role overwrite another's - and then answer for it.
BOOST_AUTO_TEST_CASE(TwoAccountsCanEachHaveARoleOfTheSameName) {

    auto repo = freshRepository();
    auto mine = roleOf("publisher", {"ens:publish-message"}, kAccount);
    auto theirs = roleOf("publisher", {"eqs:send-message"}, kOtherAccount);

    std::ignore = repo.upsertRole(mine);
    std::ignore = repo.upsertRole(theirs);

    const auto found = repo.findRoleByName(kAccount, "publisher");
    const auto other = repo.findRoleByName(kOtherAccount, "publisher");

    BOOST_REQUIRE(found.has_value());
    BOOST_REQUIRE(other.has_value());
    BOOST_TEST(found->permissions.front() == "ens:publish-message");
    BOOST_TEST(other->permissions.front() == "eqs:send-message");
}

BOOST_AUTO_TEST_CASE(AnUnknownRoleIsNotFound) {

    auto repo = freshRepository();

    BOOST_TEST(!repo.findRoleByName(kAccount, "no-such-role").has_value());
}

BOOST_AUTO_TEST_CASE(UpsertReplacesThePermissionSet) {

    auto repo = freshRepository();
    auto role = roleOf("publisher", {"ens:publish-message", "eqs:send-message"});
    std::ignore = repo.upsertRole(role);

    // update-role replaces rather than merges, so a permission taken out is taken out.
    auto narrowed = roleOf("publisher", {"ens:publish-message"});
    std::ignore = repo.upsertRole(narrowed);

    const auto found = repo.findRoleByName(kAccount, "publisher");
    BOOST_REQUIRE(found.has_value());
    BOOST_TEST(found->permissions.size() == 1U);
    BOOST_TEST(repo.countRoles(kAccount) == 1L);
}

BOOST_AUTO_TEST_CASE(RolesAreListedAndCountedPerAccount) {

    auto repo = freshRepository();
    auto mine = roleOf("publisher", {"ens:publish-message"}, kAccount);
    auto alsoMine = roleOf("reader-ish", {"ens:list-topics"}, kAccount);
    auto theirs = roleOf("publisher", {"eqs:send-message"}, kOtherAccount);
    std::ignore = repo.upsertRole(mine);
    std::ignore = repo.upsertRole(alsoMine);
    std::ignore = repo.upsertRole(theirs);

    BOOST_TEST(repo.countRoles(kAccount) == 2L);
    BOOST_TEST(repo.countRoles(kOtherAccount) == 1L);

    const auto listed = namesOf(repo.listRoles(kAccount, "", 0, 0, "name"));
    BOOST_REQUIRE(listed.size() == 2U);
    BOOST_TEST(std::ranges::contains(listed, std::string("publisher")));
    BOOST_TEST(std::ranges::contains(listed, std::string("reader-ish")));
}

BOOST_AUTO_TEST_CASE(RolesCanBeListedByPrefix) {

    auto repo = freshRepository();
    auto publisher = roleOf("topic-publisher", {"ens:publish-message"});
    auto consumer = roleOf("topic-consumer", {"ens:subscribe"});
    auto other = roleOf("queue-reader", {"eqs:list-queues"});
    std::ignore = repo.upsertRole(publisher);
    std::ignore = repo.upsertRole(consumer);
    std::ignore = repo.upsertRole(other);

    const auto listed = namesOf(repo.listRoles(kAccount, "topic-", 0, 0, "name"));

    BOOST_TEST(listed.size() == 2U);
    BOOST_TEST(!std::ranges::contains(listed, std::string("queue-reader")));
}

BOOST_AUTO_TEST_CASE(DeletingARoleRemovesItFromItsAccountOnly) {

    auto repo = freshRepository();
    auto mine = roleOf("publisher", {"ens:publish-message"}, kAccount);
    auto theirs = roleOf("publisher", {"eqs:send-message"}, kOtherAccount);
    std::ignore = repo.upsertRole(mine);
    std::ignore = repo.upsertRole(theirs);

    repo.deleteRole(kAccount, "publisher");

    BOOST_TEST(!repo.findRoleByName(kAccount, "publisher").has_value());
    BOOST_TEST(repo.findRoleByName(kOtherAccount, "publisher").has_value());
}

// ── Grants ──────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(AGrantRoundTripsWithItsScope) {

    auto repo = freshRepository();
    auto grant = grantOf("publisher", kUser, kAccount, {"production", "staging"},
                         {"ern:ens:eu-central-1:000000000000:production:topic:order-*"});

    const auto stored = repo.addGrant(grant);
    BOOST_TEST(!stored.oid.empty(), "a grant needs its id back, or it can never be revoked");

    const auto found = repo.findGrantsByPrincipals({kUser});
    BOOST_REQUIRE(found.size() == 1U);
    BOOST_TEST(found.front().role == "publisher");
    BOOST_TEST(found.front().principal == kUser);
    BOOST_TEST(found.front().grantedBy == "jens");
    BOOST_REQUIRE(found.front().namespaces.size() == 2U);
    BOOST_TEST(found.front().resources.front() == "ern:ens:eu-central-1:000000000000:production:topic:order-*");
}

// The one query authorization runs: a caller's own ERN and every group they are in, answered at
// once, because their rights are the union of all of them.
BOOST_AUTO_TEST_CASE(GrantsOfAUserAndTheirGroupsComeBackTogether) {

    auto repo = freshRepository();
    auto ofUser = grantOf("publisher", kUser);
    auto ofGroup = grantOf("consumer", kGroup);
    auto ofSomebodyElse = grantOf("operator", "ern:eam:eu-central-1:000000000000:user/someone-else");
    std::ignore = repo.addGrant(ofUser);
    std::ignore = repo.addGrant(ofGroup);
    std::ignore = repo.addGrant(ofSomebodyElse);

    const auto found = repo.findGrantsByPrincipals({kUser, kGroup});

    BOOST_TEST(found.size() == 2U);
    for (const auto &grant: found) {
        BOOST_TEST(grant.principal != "ern:eam:eu-central-1:000000000000:user/someone-else");
    }
}

// Nobody is not everybody: an empty principal list must answer with nothing rather than with every
// grant in the installation.
BOOST_AUTO_TEST_CASE(NoPrincipalsMeansNoGrants) {

    auto repo = freshRepository();
    auto grant = grantOf("publisher", kUser);
    std::ignore = repo.addGrant(grant);

    BOOST_TEST(repo.findGrantsByPrincipals({}).empty());
}

// The same role to the same principal in two namespaces is two grants, not one - collapsing them
// would silently drop one of the two scopes.
BOOST_AUTO_TEST_CASE(TheSameRoleCanBeGrantedTwiceWithDifferentScope) {

    auto repo = freshRepository();
    auto production = grantOf("publisher", kUser, kAccount, {"production"}, {"ern:...:topic:order-*"});
    auto staging = grantOf("publisher", kUser, kAccount, {"staging"}, {"*"});
    std::ignore = repo.addGrant(production);
    std::ignore = repo.addGrant(staging);

    BOOST_TEST(repo.findGrantsByPrincipals({kUser}).size() == 2U);
}

BOOST_AUTO_TEST_CASE(GrantsOfARoleAreFoundForWhoCanDoThis) {

    auto repo = freshRepository();
    auto one = grantOf("publisher", kUser);
    auto two = grantOf("publisher", kGroup);
    auto other = grantOf("consumer", kUser);
    std::ignore = repo.addGrant(one);
    std::ignore = repo.addGrant(two);
    std::ignore = repo.addGrant(other);

    BOOST_TEST(repo.findGrantsByRole(kAccount, "publisher").size() == 2U);
    BOOST_TEST(repo.findGrantsByRole(kAccount, "consumer").size() == 1U);
    BOOST_TEST(repo.findGrantsByRole(kOtherAccount, "publisher").empty());
}

// ── Listing grants ──────────────────────────────────────────────────────────

// The three questions one method answers, chosen by which argument is filled in - and the one that
// is deliberately not account-scoped, because a principal ERN names one holder wherever their
// grants apply.
BOOST_AUTO_TEST_CASE(ListingGrantsFiltersByPrincipalByRoleOrByAccount) {

    auto repo = freshRepository();
    auto mine = grantOf("publisher", kUser);
    auto theirs = grantOf("publisher", kGroup);
    auto other = grantOf("consumer", kUser);
    auto elsewhere = grantOf("publisher", kUser, kOtherAccount);
    for (auto *grant: {&mine, &theirs, &other, &elsewhere}) std::ignore = repo.addGrant(*grant);

    BOOST_TEST(repo.listGrants(kUser, "", kAccount, 0, 0, "role").size() == 3U);
    BOOST_TEST(repo.listGrants("", "publisher", kAccount, 0, 0, "role").size() == 2U);
    BOOST_TEST(repo.listGrants("", "", kAccount, 0, 0, "role").size() == 3U);
    BOOST_TEST(repo.listGrants("", "", kOtherAccount, 0, 0, "role").size() == 1U);
}

// A page and the total it is reported with have to describe the same set, or the total says
// nothing about whether there is another page.
BOOST_AUTO_TEST_CASE(CountingGrantsIgnoresPagingAndMatchesTheFilter) {

    auto repo = freshRepository();
    for (int i = 0; i < 5; ++i) {
        auto grant = grantOf("publisher", kUser, kAccount, {"namespace-" + std::to_string(i)});
        std::ignore = repo.addGrant(grant);
    }
    auto consumer = grantOf("consumer", kGroup);
    std::ignore = repo.addGrant(consumer);

    BOOST_TEST(repo.listGrants("", "", kAccount, 2, 0, "principal").size() == 2U);
    BOOST_TEST(repo.countGrants("", "", kAccount) == 6L);
    BOOST_TEST(repo.countGrants("", "publisher", kAccount) == 5L);
    BOOST_TEST(repo.countGrants(kGroup, "", kAccount) == 1L);
}

// Paging an unordered collection can show the same row on two pages and never show another, so
// the pages have to partition the set.
BOOST_AUTO_TEST_CASE(PagesOfGrantsDoNotOverlapOrLoseAny) {

    auto repo = freshRepository();
    for (int i = 0; i < 5; ++i) {
        auto grant = grantOf("role-" + std::to_string(i), kUser);
        std::ignore = repo.addGrant(grant);
    }

    std::vector<std::string> seen;
    for (long page = 0; page < 3; ++page) {
        for (const auto &grant: repo.listGrants("", "", kAccount, 2, page, "role")) seen.push_back(grant.role);
    }

    BOOST_REQUIRE(seen.size() == 5U);
    BOOST_TEST(std::ranges::is_sorted(seen));
    BOOST_TEST((std::ranges::adjacent_find(seen) == seen.end()));
}

// What a caller that predates paging sends, and what it has always got back.
BOOST_AUTO_TEST_CASE(APageSizeOfZeroIsEveryGrant) {

    auto repo = freshRepository();
    for (int i = 0; i < 5; ++i) {
        auto grant = grantOf("role-" + std::to_string(i), kUser);
        std::ignore = repo.addGrant(grant);
    }

    BOOST_TEST(repo.listGrants("", "", kAccount, 0, 0, "role").size() == 5U);
}

BOOST_AUTO_TEST_CASE(GrantsCanBeSortedInEitherDirection) {

    auto repo = freshRepository();
    auto first = grantOf("aardvark", kUser);
    auto last = grantOf("zebra", kUser);
    std::ignore = repo.addGrant(first);
    std::ignore = repo.addGrant(last);

    BOOST_TEST(repo.listGrants("", "", kAccount, 0, 0, "role", "asc").front().role == "aardvark");
    BOOST_TEST(repo.listGrants("", "", kAccount, 0, 0, "role", "desc").front().role == "zebra");
}

BOOST_AUTO_TEST_CASE(AGrantIsRevokedByItsOwnId) {

    auto repo = freshRepository();
    auto production = grantOf("publisher", kUser, kAccount, {"production"});
    auto staging = grantOf("publisher", kUser, kAccount, {"staging"});
    const auto stored = repo.addGrant(production);
    std::ignore = repo.addGrant(staging);

    repo.deleteGrant(stored.oid);

    const auto remaining = repo.findGrantsByPrincipals({kUser});
    BOOST_REQUIRE(remaining.size() == 1U);
    BOOST_TEST(remaining.front().namespaces.front() == "staging");
}

// Without this, deleting a user and creating another with the same ID would hand the new one the
// old one's rights.
BOOST_AUTO_TEST_CASE(DeletingAPrincipalTakesItsGrantsWithIt) {

    auto repo = freshRepository();
    auto one = grantOf("publisher", kUser);
    auto two = grantOf("consumer", kUser);
    auto ofGroup = grantOf("operator", kGroup);
    std::ignore = repo.addGrant(one);
    std::ignore = repo.addGrant(two);
    std::ignore = repo.addGrant(ofGroup);

    repo.deleteGrantsByPrincipal(kUser);

    BOOST_TEST(repo.findGrantsByPrincipals({kUser}).empty());
    BOOST_TEST(repo.findGrantsByPrincipals({kGroup}).size() == 1U);
}
