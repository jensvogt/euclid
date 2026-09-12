// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE AuthorizationTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <string>
#include <vector>

// Euclid includes
#include <euclid/core/BuiltinRoles.h>
#include <euclid/database/Authorization.h>

using Euclid::Core::BuiltinRoles;
using Euclid::Database::Authorization;
using Euclid::Database::AuthorizationRequest;
using Euclid::Database::Entity::EAM::Grant;

// Every request in a euclid that enforces roles is decided here, so this is where deny-by-default
// either holds or does not. What is pinned: a grant narrows by account, by namespace and by
// resource, all three independently; nothing outside a grant grants anything; and the two modules
// that are not in the permission vocabulary cannot be reached however wide the grant.

namespace {

    constexpr auto kAccount = "000000000000";
    constexpr auto kOtherAccount = "111111111111";
    constexpr auto kUser = "ern:eam:eu-central-1:000000000000:user/order-service";
    constexpr auto kTopic = "ern:ens:eu-central-1:000000000000:production:topic:order-events";

    // Built-in roles only, which is what a grant resolves against when the account has none of its
    // own. An unknown name answers nullopt, the way a deleted role would.
    Authorization::RoleLookup builtinRoles() {
        return [](const std::string &, const std::string &role) -> std::optional<std::vector<std::string>> {
            if (!BuiltinRoles::Exists(role)) return std::nullopt;
            return BuiltinRoles::PermissionsOf(role);
        };
    }

    Grant grantOf(const std::string &role, const std::vector<std::string> &namespaces = {"*"},
                  const std::vector<std::string> &resources = {"*"}, const std::string &accountId = kAccount) {
        return {.role = role, .principal = kUser, .accountId = accountId,
                .namespaces = namespaces, .resources = resources, .grantedBy = "jens"};
    }

    AuthorizationRequest publishTo(const std::string &nameSpace = "production", const std::string &resourceErn = {}) {
        return {.target = "ens", .action = "publish-message", .accountId = kAccount,
                .nameSpace = nameSpace, .resourceErn = resourceErn};
    }

    bool allows(const AuthorizationRequest &request, const std::vector<Grant> &grants) {
        return Authorization::Allows(request, grants, builtinRoles()).allowed;
    }

}// namespace

// ── Deny by default ─────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(NoGrantsMeansNo) {

    // The whole point of the redesign: today an empty grant list means unrestricted.
    const auto result = Authorization::Allows(publishTo(), {}, builtinRoles());

    BOOST_TEST(!result.allowed);
    BOOST_TEST(result.reason.find("no grant applies") != std::string::npos, "unhelpful reason: " + result.reason);
}

BOOST_AUTO_TEST_CASE(AGrantOfTheRightRoleAllows) {

    const auto result = Authorization::Allows(publishTo(), {grantOf(std::string(BuiltinRoles::Publisher))}, builtinRoles());

    BOOST_TEST(result.allowed);
    BOOST_TEST(result.role == std::string(BuiltinRoles::Publisher));
    BOOST_TEST(result.reason.find("publisher") != std::string::npos, "unhelpful reason: " + result.reason);
}

BOOST_AUTO_TEST_CASE(AGrantOfTheWrongRoleRefusesAndSaysWhy) {

    // The role applies here; it simply does not hold this permission. A different problem from
    // having no grant at all, and the reason says so.
    const auto result = Authorization::Allows(publishTo(), {grantOf(std::string(BuiltinRoles::Consumer))}, builtinRoles());

    BOOST_TEST(!result.allowed);
    BOOST_TEST(result.reason.find("no role granted here holds") != std::string::npos, "unhelpful reason: " + result.reason);
    BOOST_TEST(result.reason.find("ens:publish-message") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(GrantsAreAUnionSoOneOfSeveralIsEnough) {

    const std::vector grants{
            grantOf(std::string(BuiltinRoles::Reader)),
            grantOf(std::string(BuiltinRoles::Consumer)),
            grantOf(std::string(BuiltinRoles::Publisher)),
    };

    BOOST_TEST(allows(publishTo(), grants));
}

// ── Account scope ───────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(AGrantInAnotherAccountDoesNotApply) {

    const auto grant = grantOf(std::string(BuiltinRoles::AccountAdministrator), {"*"}, {"*"}, kOtherAccount);

    BOOST_TEST(!allows(publishTo(), {grant}));
}

// Roles are per account, so there is no wildcard account on a grant - the only cross-account
// principals are the administrator group and the system principal, and neither is a grant.
BOOST_AUTO_TEST_CASE(ThereIsNoWildcardAccount) {

    const auto grant = grantOf(std::string(BuiltinRoles::AccountAdministrator), {"*"}, {"*"}, "*");

    BOOST_TEST(!allows(publishTo(), {grant}));
}

// ── Namespace scope ─────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(ANamespaceGrantAppliesOnlyThere) {

    const auto grant = grantOf(std::string(BuiltinRoles::Publisher), {"production"});

    BOOST_TEST(allows(publishTo("production"), {grant}));
    BOOST_TEST(!allows(publishTo("development"), {grant}));
}

BOOST_AUTO_TEST_CASE(TheNamespaceWildcardCoversEveryNamespace) {

    const auto grant = grantOf(std::string(BuiltinRoles::Publisher), {"*"});

    BOOST_TEST(allows(publishTo("production"), {grant}));
    BOOST_TEST(allows(publishTo("development"), {grant}));
    BOOST_TEST(allows(publishTo(""), {grant}));
}

// The account root is a namespace like any other, not "all of them" - so a grant has to name it.
BOOST_AUTO_TEST_CASE(TheAccountRootIsANamespaceOfItsOwn) {

    const auto scoped = grantOf(std::string(BuiltinRoles::Publisher), {"production"});
    BOOST_TEST(!allows(publishTo(""), {scoped}));

    const auto root = grantOf(std::string(BuiltinRoles::Publisher), {""});
    BOOST_TEST(allows(publishTo(""), {root}));
    BOOST_TEST(!allows(publishTo("production"), {root}));
}

BOOST_AUTO_TEST_CASE(AGrantWithNoNamespacesGrantsNothing) {

    const auto grant = grantOf(std::string(BuiltinRoles::Publisher), {});

    BOOST_TEST(!allows(publishTo("production"), {grant}));
    BOOST_TEST(!allows(publishTo(""), {grant}));
}

// ── Resource scope ──────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(AnActionThatNamesNoResourceIsNotNarrowedByOne) {

    // Most actions name none. A grant's resource list then has nothing to filter.
    const auto grant = grantOf(std::string(BuiltinRoles::Publisher), {"*"}, {"ern:ens:...:topic:something-else"});

    BOOST_TEST(allows(publishTo("production"), {grant}));
}

BOOST_AUTO_TEST_CASE(AResourcePatternNarrowsTheActionsThatNameOne) {

    const auto grant = grantOf(std::string(BuiltinRoles::Publisher), {"*"}, {"ern:ens:eu-central-1:000000000000:production:topic:order-*"});

    BOOST_TEST(allows(publishTo("production", kTopic), {grant}));
    BOOST_TEST(!allows(publishTo("production", "ern:ens:eu-central-1:000000000000:production:topic:payroll"), {grant}));
}

BOOST_AUTO_TEST_CASE(ResourceMatchingIsExactOrATrailingStar) {

    BOOST_TEST(Authorization::ResourceMatches("*", kTopic));
    BOOST_TEST(Authorization::ResourceMatches(kTopic, kTopic));
    BOOST_TEST(Authorization::ResourceMatches("ern:ens:eu-central-1:000000000000:production:topic:order-*", kTopic));

    BOOST_TEST(!Authorization::ResourceMatches("ern:ens:eu-central-1:000000000000:production:topic:payroll", kTopic));
    BOOST_TEST(!Authorization::ResourceMatches("ern:ens:eu-central-1:111111111111:production:topic:order-*", kTopic));
}

// A star in the middle is not a pattern language this understands, and must not be read as one -
// treating it as a prefix would silently widen the grant to everything before the star.
BOOST_AUTO_TEST_CASE(OnlyATrailingStarIsAWildcard) {

    BOOST_TEST(!Authorization::ResourceMatches("ern:ens:*:topic:order-events", kTopic));
    BOOST_TEST(!Authorization::ResourceMatches("*:topic:order-events", kTopic));
    BOOST_TEST(!Authorization::ResourceMatches("", kTopic));
}

BOOST_AUTO_TEST_CASE(AGrantWithNoResourcesCoversNoNamedResource) {

    const auto grant = grantOf(std::string(BuiltinRoles::Publisher), {"*"}, {});

    BOOST_TEST(!allows(publishTo("production", kTopic), {grant}));
    // ...but still covers the actions that name none.
    BOOST_TEST(allows(publishTo("production"), {grant}));
}

// ── The unbindable modules ──────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(NoGrantReachesEmmOrEmd) {

    const auto everything = grantOf(std::string(BuiltinRoles::AccountAdministrator));

    const AuthorizationRequest emm{.target = "emm", .action = "import", .accountId = kAccount, .nameSpace = "production"};
    const AuthorizationRequest emd{.target = "emd", .action = "replace-one", .accountId = kAccount, .nameSpace = "production"};

    const auto emmResult = Authorization::Allows(emm, {everything}, builtinRoles());
    BOOST_TEST(!emmResult.allowed);
    BOOST_TEST(emmResult.reason.find("is not a permission any role can hold") != std::string::npos);

    BOOST_TEST(!Authorization::Allows(emd, {everything}, builtinRoles()).allowed);
}

BOOST_AUTO_TEST_CASE(AnActionNoModuleDispatchesIsRefused) {

    // A handler that mistypes its own action name fails closed rather than matching a wildcard.
    const auto everything = grantOf(std::string(BuiltinRoles::AccountAdministrator));
    const AuthorizationRequest typo{.target = "ens", .action = "publish-mesage", .accountId = kAccount, .nameSpace = "production"};

    BOOST_TEST(!Authorization::Allows(typo, {everything}, builtinRoles()).allowed);
}

// ── Role resolution ─────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(AGrantNamingAMissingRoleIsSkippedNotHonoured) {

    // A role deleted out from under its grants must not become "grants everything" or an error that
    // takes the whole request down - the grant simply does not apply.
    const std::vector grants{grantOf("deleted-role"), grantOf(std::string(BuiltinRoles::Publisher))};

    BOOST_TEST(allows(publishTo(), grants));
    BOOST_TEST(!allows(publishTo(), {grantOf("deleted-role")}));
}

BOOST_AUTO_TEST_CASE(AnAccountsOwnRoleIsResolvedBeforeTheBuiltins) {

    // What a stored role looks like to this: the lookup answers for it, and the built-ins are the
    // fallback rather than the other way round.
    const auto roles = [](const std::string &accountId, const std::string &role) -> std::optional<std::vector<std::string>> {
        if (accountId == kAccount && role == "topic-publisher") return std::vector<std::string>{"ens:publish-message"};
        if (BuiltinRoles::Exists(role)) return BuiltinRoles::PermissionsOf(role);
        return std::nullopt;
    };

    BOOST_TEST(Authorization::Allows(publishTo(), {grantOf("topic-publisher")}, roles).allowed);

    // The same role name in another account is a different role, and this one does not exist there.
    const auto elsewhere = grantOf("topic-publisher", {"*"}, {"*"}, kOtherAccount);
    BOOST_TEST(!Authorization::Allows(publishTo(), {elsewhere}, roles).allowed);
}

// ── The three the role concept opens with ───────────────────────────────────

BOOST_AUTO_TEST_CASE(TheThreeRightsTheConceptWasAskedFor) {

    // "ENS topic create, message publish, message subscribe", granted separately.
    const auto roles = [](const std::string &, const std::string &role) -> std::optional<std::vector<std::string>> {
        if (role == "topic-creator") return std::vector<std::string>{"ens:create-topic"};
        if (role == "topic-publisher") return std::vector<std::string>{"ens:publish-message"};
        if (role == "topic-subscriber") return std::vector<std::string>{"ens:subscribe"};
        return std::nullopt;
    };

    const AuthorizationRequest create{.target = "ens", .action = "create-topic", .accountId = kAccount, .nameSpace = "production"};
    const AuthorizationRequest publish{.target = "ens", .action = "publish-message", .accountId = kAccount, .nameSpace = "production"};
    const AuthorizationRequest subscribe{.target = "ens", .action = "subscribe", .accountId = kAccount, .nameSpace = "production"};

    const std::vector publisher{grantOf("topic-publisher")};
    BOOST_TEST(Authorization::Allows(publish, publisher, roles).allowed);
    BOOST_TEST(!Authorization::Allows(create, publisher, roles).allowed);
    BOOST_TEST(!Authorization::Allows(subscribe, publisher, roles).allowed);

    const std::vector subscriber{grantOf("topic-subscriber")};
    BOOST_TEST(Authorization::Allows(subscribe, subscriber, roles).allowed);
    BOOST_TEST(!Authorization::Allows(publish, subscriber, roles).allowed);

    const std::vector creator{grantOf("topic-creator")};
    BOOST_TEST(Authorization::Allows(create, creator, roles).allowed);
    BOOST_TEST(!Authorization::Allows(publish, creator, roles).allowed);
}
