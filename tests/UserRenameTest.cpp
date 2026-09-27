// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE UserRenameTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <algorithm>
#include <string>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/core/ErnUtils.h>
#include <euclid/database/Database.h>
#include <euclid/database/RepositoryFactory.h>
#include <euclid/database/repository/eam/MongoEamRepository.h>

using Euclid::Database::MongoEamRepository;
using Euclid::Database::Entity::EAM::AccessKey;
using Euclid::Database::Entity::EAM::Grant;
using Euclid::Database::Entity::EAM::User;
using Euclid::Database::Entity::EAM::UserGroup;

// A userId is not a label. The user's ERN is built from it and every grant hangs off that ERN; a
// group's membership is a list of userIds. So a rename that moved only the user row would leave
// them authenticating perfectly and holding nothing, out of every group they were in, with nothing
// anywhere saying why - which is what these cases are here to prevent, since the failure looks
// exactly like a permissions bug from the outside.

namespace {

    constexpr auto kAccount = "000000000000";

    MongoEamRepository freshRepository() {
        Euclid::Database::Database::instance().initializeMemory();
        return MongoEamRepository{};
    }

    User userOf(MongoEamRepository &repository, const std::string &userId) {
        User user;
        user.userId = userId;
        user.accountId = kAccount;
        user.region = "eu-central-1";
        user.ern = Euclid::Core::createEamUserErn(kAccount, userId);
        user.email = userId + "@example.invalid";
        user.loginEnabled = true;
        // Per user: access key ids are unique across the installation, so a second user sharing one
        // is refused by the index rather than stored.
        user.accessKeys.push_back(AccessKey{.accessKeyId = "AKIA-" + userId, .secretAccessKey = "secret", .active = true});
        return repository.upsertUser(user);
    }

    Grant grantOf(const std::string &principal, const std::string &role = "consumer") {
        return {.role = role, .principal = principal, .accountId = kAccount, .namespaces = {"development"},
                .resources = {"*"}, .granted = std::chrono::system_clock::now(), .grantedBy = "jens"};
    }

    UserGroup groupOf(MongoEamRepository &repository, const std::string &name, const std::vector<std::string> &userIds) {
        UserGroup group;
        group.name = name;
        group.ern = "ern:eam:eu-central-1:000000000000:usergroup:" + name;
        group.accountId = kAccount;
        group.region = "eu-central-1";
        group.userIds = userIds;
        return repository.upsertUserGroup(group);
    }

}// namespace

BOOST_AUTO_TEST_CASE(TheUserIsFoundUnderTheNewNameAndNotTheOld) {

    auto repository = freshRepository();
    std::ignore = userOf(repository, "jens");

    const auto renamed = repository.renameUser("jens", "jens.vogt");
    BOOST_TEST_REQUIRE(renamed.has_value());
    BOOST_TEST(renamed->userId == "jens.vogt");

    BOOST_TEST(repository.userExists("jens.vogt"));
    BOOST_TEST(!repository.userExists("jens"));

    // One user, not two: this is a rename, and upsertUser() matching on the field being changed is
    // exactly how it would have become two.
    BOOST_TEST(repository.listUsers("", 0, 0, "userId").size() == 1U);
}

BOOST_AUTO_TEST_CASE(TheErnIsRebuiltAndTheRestOfTheUserIsUntouched) {

    auto repository = freshRepository();
    std::ignore = userOf(repository, "jens");

    const auto renamed = repository.renameUser("jens", "jens.vogt");
    BOOST_TEST_REQUIRE(renamed.has_value());

    // The name is part of the ERN, so the ERN follows - and carries the account and region it
    // always did rather than being patched into something new.
    BOOST_TEST(renamed->ern == "ern:eam:eu-central-1:000000000000:user:jens.vogt");
    BOOST_TEST(renamed->accountId == kAccount);
    BOOST_TEST(renamed->region == "eu-central-1");
    BOOST_TEST(renamed->email == "jens@example.invalid");

    // The access key travels because it lives in the user document, and keeps its id - so anything
    // already signing with it goes on working, which is the point of not touching it.
    BOOST_TEST_REQUIRE(renamed->accessKeys.size() == 1U);
    BOOST_TEST(renamed->accessKeys.front().accessKeyId == "AKIA-jens");
}

BOOST_AUTO_TEST_CASE(TheGrantsComeWithTheUser) {

    auto repository = freshRepository();
    const auto user = userOf(repository, "jens");
    auto first = grantOf(user.ern, "consumer");
    auto second = grantOf(user.ern, "publisher");
    std::ignore = repository.addGrant(first);
    std::ignore = repository.addGrant(second);

    const auto renamed = repository.renameUser("jens", "jens.vogt");
    BOOST_TEST_REQUIRE(renamed.has_value());

    // Both of them, under the new ERN. A partial rewrite would leave the user holding half their
    // rights, which is harder to spot than holding none.
    BOOST_TEST(repository.findGrantsByPrincipals({renamed->ern}).size() == 2U);

    // And nothing left pointing at the name that no longer exists - grants nobody can reach
    // accumulate, and attach themselves to whoever is created under the old name next.
    BOOST_TEST(repository.findGrantsByPrincipals({user.ern}).empty());
}

BOOST_AUTO_TEST_CASE(OnlyThisUsersGrantsMove) {

    auto repository = freshRepository();
    const auto renamedUser = userOf(repository, "jens");
    const auto otherUser = userOf(repository, "anna");
    auto mine = grantOf(renamedUser.ern);
    auto theirs = grantOf(otherUser.ern);
    std::ignore = repository.addGrant(mine);
    std::ignore = repository.addGrant(theirs);

    std::ignore = repository.renameUser("jens", "jens.vogt");

    // The filter is the old ERN, so somebody else's grant is not swept up by a rename that has
    // nothing to do with them.
    BOOST_TEST(repository.findGrantsByPrincipals({otherUser.ern}).size() == 1U);
}

BOOST_AUTO_TEST_CASE(GroupMembershipFollowsTheName) {

    auto repository = freshRepository();
    std::ignore = userOf(repository, "jens");
    std::ignore = userOf(repository, "anna");
    std::ignore = groupOf(repository, "operators", {"jens", "anna"});
    std::ignore = groupOf(repository, "auditors", {"jens"});
    std::ignore = groupOf(repository, "nobody", {"anna"});

    std::ignore = repository.renameUser("jens", "jens.vogt");

    // A group's members are userIds, not ERNs, so this is the half of a rename that is easiest to
    // forget - and losing it costs every grant the user held through a group.
    const auto operators = repository.findUserGroupByName("operators");
    BOOST_TEST_REQUIRE(operators.has_value());
    BOOST_TEST(std::ranges::contains(operators->userIds, std::string("jens.vogt")));
    BOOST_TEST(!std::ranges::contains(operators->userIds, std::string("jens")));

    // The other member is left where they were.
    BOOST_TEST(std::ranges::contains(operators->userIds, std::string("anna")));

    // Every group they were in, not the first one found.
    const auto auditors = repository.findUserGroupByName("auditors");
    BOOST_TEST_REQUIRE(auditors.has_value());
    BOOST_TEST(std::ranges::contains(auditors->userIds, std::string("jens.vogt")));

    // And no group they were not in.
    const auto nobody = repository.findUserGroupByName("nobody");
    BOOST_TEST_REQUIRE(nobody.has_value());
    BOOST_TEST(nobody->userIds.size() == 1U);
    BOOST_TEST(nobody->userIds.front() == "anna");
}

// A user stored without them was stored as having been created at the epoch, which is how every
// application's technical principal came to be dated the 1st of January 1970: EAP built the user
// and set everything about it except these two. Filled in by the repository now, so that the next
// caller to forget is not a second bug.
BOOST_AUTO_TEST_CASE(AUserStoredWithoutTimestampsIsStillDated) {

    auto repository = freshRepository();

    // Floored to the millisecond, which is all BSON keeps: a stamp taken now and read back is this
    // instant with its sub-millisecond part cut off, and would otherwise compare as earlier than
    // the moment before it was taken.
    const auto before = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());

    Euclid::Database::Entity::EAM::User user;
    user.userId = "app-orders-a3f2k9x1";
    user.accountId = kAccount;
    user.region = "eu-central-1";
    user.ern = Euclid::Core::createEamUserErn(kAccount, user.userId);
    user.email = user.userId + "@euclid.invalid";
    user.loginEnabled = false;

    const auto stored = repository.upsertUser(user);

    BOOST_TEST((stored.created >= before));
    BOOST_TEST((stored.modified >= before));

    // And they survive the round trip rather than only being on the copy that was returned.
    const auto read = repository.findUserByUserId(user.userId);
    BOOST_TEST_REQUIRE(read.has_value());
    BOOST_TEST((read->created.time_since_epoch().count() > 0));
    BOOST_TEST((read->modified.time_since_epoch().count() > 0));
}

// What a caller says happened is not overwritten by what the repository would have guessed - an
// update is not a creation, and a row imported from somewhere else keeps its own history.
BOOST_AUTO_TEST_CASE(TimestampsTheCallerSetAreLeftAlone) {

    auto repository = freshRepository();
    const auto longAgo = std::chrono::system_clock::now() - std::chrono::hours(24 * 365);

    auto user = userOf(repository, "jens");
    user.created = longAgo;
    user.modified = longAgo;
    const auto stored = repository.upsertUser(user);

    // To the millisecond, which is what BSON keeps.
    BOOST_TEST((std::chrono::duration_cast<std::chrono::milliseconds>(stored.created - longAgo).count() == 0));
    BOOST_TEST((std::chrono::duration_cast<std::chrono::milliseconds>(stored.modified - longAgo).count() == 0));
}

BOOST_AUTO_TEST_CASE(RenamingSomebodyWhoIsNotThereChangesNothing) {

    auto repository = freshRepository();
    std::ignore = userOf(repository, "jens");

    // std::nullopt rather than an exception: "no such user" is an answer, and the handler turns it
    // into a 404 rather than a 500.
    BOOST_TEST(!repository.renameUser("nobody", "somebody").has_value());
    BOOST_TEST(repository.userExists("jens"));
    BOOST_TEST(!repository.userExists("somebody"));
}
