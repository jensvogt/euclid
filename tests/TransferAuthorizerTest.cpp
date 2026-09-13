// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE TransferAuthorizerTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <algorithm>
#include <chrono>
#include <string>
#include <tuple>
#include <vector>

// Euclid includes
#include <TransferAuthorizer.h>
#include <euclid/core/BuiltinRoles.h>
#include <euclid/database/Database.h>
#include <euclid/database/RepositoryFactory.h>

using Euclid::Core::BuiltinRoles;
using Euclid::Database::Entity::EAM::Grant;
using Euclid::Database::Entity::EAM::User;
using Euclid::Database::Entity::EAM::UserGroup;
using Euclid::Database::Entity::ETS::TransferServer;
using Euclid::Transfer::TransferAuthorizer;
using Euclid::Transfer::TransferIdentity;

// An FTP or SFTP client used to be authorized by one bit - whether the transfer server listed it -
// and everything it could then do was everything the protocol offers. This is where the second
// question is answered, so what it pins is that the answer comes from the same grants as the rest
// of euclid and defaults to no.
//
// The account, namespace and resource the check is made against are chosen here rather than by the
// caller, and getting any of the three wrong fails open in a way no FTP client would report: a
// resourceErn of "" would make every server-scoped grant match every server.

namespace {

    constexpr auto kAccount = "000000000000";
    constexpr auto kOtherAccount = "111111111111";
    constexpr auto kRegion = "eu-central-1";

    constexpr auto kServerErn = "ern:ets:eu-central-1:000000000000:production:server:incoming";
    constexpr auto kOtherServerErn = "ern:ets:eu-central-1:000000000000:production:server:outgoing";

    // Every case makes its own user id. The authorizer reads through Database::UsersByUserId(),
    // a process-wide TTL cache that no test can clear, so sharing an id between cases would let
    // one case's user answer another's lookup.
    int nextId() {
        static int counter = 0;
        return ++counter;
    }

    TransferServer serverOf(const std::string &ern = kServerErn, const std::string &nameSpace = "production",
                            const std::string &accountId = kAccount) {
        TransferServer server;
        server.serverId = "incoming";
        server.ern = ern;
        server.accountId = accountId;
        server.nameSpace = nameSpace;
        server.region = kRegion;
        return server;
    }

    // A user in the store, and the identity a login would have produced for them.
    TransferIdentity userWithGrants(const std::vector<Grant> &grants, const std::vector<std::string> &groups = {}) {

        const auto userId = "supplier-" + std::to_string(nextId());
        const auto repo = Euclid::Database::RepositoryFactory::instance().eamRepository();

        User user;
        user.userId = userId;
        user.ern = "ern:eam:eu-central-1:000000000000:user:" + userId;
        // Distinct, because the store has a unique index on it and every user here would otherwise
        // share the empty string.
        user.email = userId + "@example.com";
        user.accountId = kAccount;
        user.region = kRegion;
        std::ignore = repo->upsertUser(user);

        for (const auto &groupName: groups) {
            UserGroup group;
            group.name = groupName;
            group.ern = "ern:eam:eu-central-1:000000000000:userGroup:" + groupName;
            group.accountId = kAccount;
            group.region = kRegion;
            group.userIds = {userId};
            std::ignore = repo->upsertUserGroup(group);
        }

        for (auto grant: grants) {
            if (grant.principal.empty()) grant.principal = user.ern;
            std::ignore = repo->addGrant(grant);
        }

        return {.userId = userId, .accountId = kAccount, .token = "not-used-here"};
    }

    Grant grantOf(const std::string &role, const std::vector<std::string> &namespaces = {"*"},
                  const std::vector<std::string> &resources = {"*"}, const std::string &accountId = kAccount) {
        return {.role = role, .principal = {}, .accountId = accountId, .namespaces = namespaces,
                .resources = resources, .granted = std::chrono::system_clock::now(), .grantedBy = "jens"};
    }

    struct MemoryStore {
        MemoryStore() {
            Euclid::Database::Database::instance().initializeMemory();
            Euclid::Database::RepositoryFactory::instance().initialize(Euclid::Database::BackendType::MEMORY);
        }
    };

}// namespace

BOOST_GLOBAL_FIXTURE(MemoryStore);

// ── Deny by default ─────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(AUserWithNoGrantMayDoNothing) {

    // Being allowed to log in is what the transfer server's userIds decided; it is not a grant,
    // and on its own it now buys nothing.
    const auto identity = userWithGrants({});
    const TransferAuthorizer authorizer(serverOf());

    for (const auto &action: {"list-directory", "get-file", "put-file", "delete-file",
                              "create-directory", "delete-directory", "rename-file"}) {
        const auto decision = authorizer.Allows(identity, action);
        BOOST_TEST(!decision.allowed, std::string("a user with no grant was allowed ") + action);
        BOOST_TEST(!decision.reason.empty(), "a refusal with no reason tells the operator nothing");
    }
}

BOOST_AUTO_TEST_CASE(AnActionOutsideTheVocabularyIsRefusedHowerverWideTheGrant) {

    // *:* and still no. A mistyped action in a session handler fails closed rather than matching
    // the wildcard, which is the property that keeps a typo from becoming an opening.
    const auto identity = userWithGrants({grantOf(std::string(BuiltinRoles::AccountAdministrator))});
    const TransferAuthorizer authorizer(serverOf());

    BOOST_TEST(!authorizer.Allows(identity, "get-fil").allowed);
    BOOST_TEST(!authorizer.Allows(identity, "chmod").allowed);
    BOOST_TEST(!authorizer.Allows(identity, "").allowed);
}

// ── The built-in transfer role ──────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(TheTransferRoleAllowsEveryCommand) {

    const auto identity = userWithGrants({grantOf(std::string(BuiltinRoles::Transfer))});
    const TransferAuthorizer authorizer(serverOf());

    for (const auto &action: {"list-directory", "get-file", "put-file", "delete-file",
                              "create-directory", "delete-directory", "rename-file"}) {
        BOOST_TEST(authorizer.Allows(identity, action).allowed,
                   std::string("the transfer role does not allow ") + action);
    }
}

BOOST_AUTO_TEST_CASE(AReadOnlyClientCanListAndDownloadAndNothingElse) {

    // The point of the whole exercise: a supplier who may collect files but not leave or remove
    // any. `reader` says that without anybody writing a role.
    const auto identity = userWithGrants({grantOf(std::string(BuiltinRoles::Reader))});
    const TransferAuthorizer authorizer(serverOf());

    BOOST_TEST(authorizer.Allows(identity, "list-directory").allowed);
    BOOST_TEST(authorizer.Allows(identity, "get-file").allowed);

    BOOST_TEST(!authorizer.Allows(identity, "put-file").allowed);
    BOOST_TEST(!authorizer.Allows(identity, "delete-file").allowed);
    BOOST_TEST(!authorizer.Allows(identity, "delete-directory").allowed);
    BOOST_TEST(!authorizer.Allows(identity, "rename-file").allowed);
}

BOOST_AUTO_TEST_CASE(AnOperatorCanWriteButNotDelete) {

    const auto identity = userWithGrants({grantOf(std::string(BuiltinRoles::Operator))});
    const TransferAuthorizer authorizer(serverOf());

    BOOST_TEST(authorizer.Allows(identity, "put-file").allowed);
    BOOST_TEST(authorizer.Allows(identity, "create-directory").allowed);
    BOOST_TEST(authorizer.Allows(identity, "rename-file").allowed);

    BOOST_TEST(!authorizer.Allows(identity, "delete-file").allowed);
    BOOST_TEST(!authorizer.Allows(identity, "delete-directory").allowed);
}

// ── Scope ───────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(AGrantScopedToOneServerDoesNotReachAnother) {

    // The check the authorizer makes on the caller's behalf: the server's own ERN is the resource.
    // Passing no resource here would make this grant answer for every server in the account, and
    // nothing downstream would notice.
    const auto identity = userWithGrants({grantOf(std::string(BuiltinRoles::Transfer), {"*"}, {kServerErn})});

    BOOST_TEST(TransferAuthorizer(serverOf(kServerErn)).Allows(identity, "put-file").allowed);
    BOOST_TEST(!TransferAuthorizer(serverOf(kOtherServerErn)).Allows(identity, "put-file").allowed);
}

BOOST_AUTO_TEST_CASE(AGrantInAnotherNamespaceDoesNotReachThisServer) {

    const auto identity = userWithGrants({grantOf(std::string(BuiltinRoles::Transfer), {"staging"})});

    BOOST_TEST(TransferAuthorizer(serverOf(kServerErn, "staging")).Allows(identity, "put-file").allowed);
    BOOST_TEST(!TransferAuthorizer(serverOf(kServerErn, "production")).Allows(identity, "put-file").allowed);
}

BOOST_AUTO_TEST_CASE(AGrantInAnotherAccountDoesNotReachThisServer) {

    const auto identity = userWithGrants({grantOf(std::string(BuiltinRoles::Transfer), {"*"}, {"*"}, kOtherAccount)});
    const TransferAuthorizer authorizer(serverOf());

    BOOST_TEST(!authorizer.Allows(identity, "put-file").allowed);
}

BOOST_AUTO_TEST_CASE(APrefixPatternReachesTheServersItNames) {

    const auto identity = userWithGrants(
            {grantOf(std::string(BuiltinRoles::Transfer), {"*"}, {"ern:ets:eu-central-1:000000000000:production:server:in*"})});

    BOOST_TEST(TransferAuthorizer(serverOf(kServerErn)).Allows(identity, "get-file").allowed);
    BOOST_TEST(!TransferAuthorizer(serverOf(kOtherServerErn)).Allows(identity, "get-file").allowed);
}

// ── Where the grant hangs ───────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(AGrantToAGroupReachesItsMembers) {

    // How a transfer server is actually administered: the server lists a user group, and the same
    // group is what the role is granted to - so adding a supplier stays one operation.
    const auto identity = userWithGrants({}, {"suppliers-" + std::to_string(nextId())});
    const auto repo = Euclid::Database::RepositoryFactory::instance().eamRepository();

    const auto groups = repo->listUserGroups("", 0, 0, "name");
    const auto group = std::ranges::find_if(groups, [&](const auto &g) {
        return std::ranges::contains(g.userIds, identity.userId);
    });
    BOOST_REQUIRE(group != groups.end());

    const TransferAuthorizer authorizer(serverOf());
    BOOST_TEST(!authorizer.Allows(identity, "put-file").allowed);

    auto grant = grantOf(std::string(BuiltinRoles::Transfer));
    grant.principal = group->ern;
    std::ignore = repo->addGrant(grant);

    BOOST_TEST(authorizer.Allows(identity, "put-file").allowed);
}

BOOST_AUTO_TEST_CASE(AnUnknownUserIsRefusedRatherThanPassedOn) {

    // The HTTP gate answers "left to the handler" here, because a handler behind it will say 401.
    // There is no handler behind this one, so an account deleted mid-session has to be a refusal.
    const TransferAuthorizer authorizer(serverOf());
    const TransferIdentity ghost{.userId = "deleted-while-logged-in", .accountId = kAccount, .token = "t"};

    const auto decision = authorizer.Allows(ghost, "list-directory");
    BOOST_TEST(!decision.allowed);
    BOOST_TEST(decision.reason.find("no EAM user") != std::string::npos);
}
