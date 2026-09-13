// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE BuiltinRoleTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <algorithm>
#include <string>

// Euclid includes
#include <euclid/core/BuiltinRoles.h>
#include <euclid/core/Permissions.h>

using Euclid::Core::BuiltinRoles;
using Euclid::Core::Permissions;

// The built-in roles are the only roles that are not stored, so they are the only ones that can be
// wrong without an administrator having written them that way. Two things have to hold: every
// permission they name has to exist - a renamed action would otherwise empty a role silently - and
// the rules that compute them have to keep meaning what they say as the vocabulary grows.

namespace {

    bool grants(const std::string_view role, const std::string_view permission) {
        return std::ranges::any_of(BuiltinRoles::PermissionsOf(role), [&](const auto &granted) {
            return Permissions::Matches(granted, permission);
        });
    }

    // The entries of a role that are not wildcards, which are the ones that must name real actions.
    std::vector<std::string> literalsOf(const std::string_view role) {
        std::vector<std::string> literals;
        for (const auto &permission: BuiltinRoles::PermissionsOf(role)) {
            if (!permission.ends_with(":*") && permission != Permissions::Everything) literals.push_back(permission);
        }
        return literals;
    }

}// namespace

BOOST_AUTO_TEST_CASE(EveryBuiltinRoleNamesOnlyRealPermissions) {

    // The failure this prevents: an action is renamed, a role keeps naming the old one, and
    // everybody bound to it silently loses the right it was granted for.
    for (const auto &role: BuiltinRoles::Names()) {
        for (const auto &permission: literalsOf(role)) {
            BOOST_TEST(Permissions::Exists(permission),
                       "built-in role '" + role + "' grants '" + permission + "', which no module dispatches");
        }
    }
}

BOOST_AUTO_TEST_CASE(EveryBuiltinRoleGrantsSomething) {

    for (const auto &role: BuiltinRoles::Names()) {
        BOOST_TEST(!BuiltinRoles::PermissionsOf(role).empty(), "built-in role '" + role + "' grants nothing");
    }
}

BOOST_AUTO_TEST_CASE(EveryBuiltinRoleIsDescribedAndFound) {

    for (const auto &role: BuiltinRoles::Names()) {
        BOOST_TEST(BuiltinRoles::Exists(role));
        BOOST_TEST(!BuiltinRoles::DescriptionOf(role).empty(), "built-in role '" + role + "' has no description");
    }

    BOOST_TEST(!BuiltinRoles::Exists("not-a-role"));
    BOOST_TEST(BuiltinRoles::PermissionsOf("not-a-role").empty());
    BOOST_TEST(BuiltinRoles::DescriptionOf("not-a-role").empty());
}

// There is no `administrator` role: installation administration is user-group membership, because
// roles are per account and EMM has to be reachable by something no role can name.
BOOST_AUTO_TEST_CASE(ThereIsNoAdministratorRole) {

    BOOST_TEST(!BuiltinRoles::Exists("administrator"));
    BOOST_TEST(!std::ranges::contains(BuiltinRoles::Names(), std::string("administrator")));
}

// ── account-administrator ───────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(AccountAdministratorGrantsEveryPermission) {

    for (const auto &permission: Permissions::All()) {
        BOOST_TEST(grants(BuiltinRoles::AccountAdministrator, permission), permission + " is not granted by account-administrator");
    }
}

BOOST_AUTO_TEST_CASE(AccountAdministratorStillDoesNotReachTheUnbindableModules) {

    // The strongest role there is, and it is still not installation administration.
    BOOST_TEST(!grants(BuiltinRoles::AccountAdministrator, "emm:import"));
    BOOST_TEST(!grants(BuiltinRoles::AccountAdministrator, "emd:replace-one"));
}

// ── operator ────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(OperatorRunsThingsButDoesNotRemoveThem) {

    BOOST_TEST(grants(BuiltinRoles::Operator, "eqs:create-queue"));
    BOOST_TEST(grants(BuiltinRoles::Operator, "eqs:send-message"));
    BOOST_TEST(grants(BuiltinRoles::Operator, "ens:start-topic"));
    BOOST_TEST(grants(BuiltinRoles::Operator, "eap:start-application"));
    BOOST_TEST(grants(BuiltinRoles::Operator, "eap:redeploy-application"));

    BOOST_TEST(!grants(BuiltinRoles::Operator, "eqs:delete-queue"));
    BOOST_TEST(!grants(BuiltinRoles::Operator, "ens:delete-topic"));
    BOOST_TEST(!grants(BuiltinRoles::Operator, "ens:purge-topic"));
    BOOST_TEST(!grants(BuiltinRoles::Operator, "esm:purge-bucket"));
}

BOOST_AUTO_TEST_CASE(OperatorDoesNotAdministerAccess) {

    // Running the installation and deciding who may use it are different jobs.
    BOOST_TEST(!grants(BuiltinRoles::Operator, "eam:register"));
    BOOST_TEST(!grants(BuiltinRoles::Operator, "eam:create-account"));
    BOOST_TEST(!grants(BuiltinRoles::Operator, "eam:list-users"));
}

// The rule is applied to the vocabulary rather than transcribed from it, so this holds for actions
// nobody has written yet.
BOOST_AUTO_TEST_CASE(OperatorIsExactlyTheRuleItClaims) {

    for (const auto &permission: Permissions::All()) {
        const auto action = permission.substr(permission.find(':') + 1);
        const bool destructive = action.starts_with("delete-") || action.starts_with("purge-");
        const bool accessManagement = permission.starts_with("eam:");

        BOOST_TEST(grants(BuiltinRoles::Operator, permission) == (!destructive && !accessManagement),
                   permission + " is on the wrong side of the operator rule");
    }
}

// ── reader ──────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(ReaderOnlyReads) {

    BOOST_TEST(grants(BuiltinRoles::Reader, "eqs:list-queues"));
    BOOST_TEST(grants(BuiltinRoles::Reader, "ens:get-topic-metadata"));
    BOOST_TEST(grants(BuiltinRoles::Reader, "ekv:describe-table"));

    BOOST_TEST(!grants(BuiltinRoles::Reader, "ens:publish-message"));
    BOOST_TEST(!grants(BuiltinRoles::Reader, "eqs:send-message"));
    BOOST_TEST(!grants(BuiltinRoles::Reader, "esm:delete-bucket"));
    BOOST_TEST(!grants(BuiltinRoles::Reader, "ens:create-topic"));
}

BOOST_AUTO_TEST_CASE(ReaderIsExactlyTheRuleItClaims) {

    for (const auto &permission: Permissions::All()) {
        const auto action = permission.substr(permission.find(':') + 1);
        const bool reads = action.starts_with("list-") || action.starts_with("get-")
                           || action.starts_with("describe-") || action.starts_with("count-");

        BOOST_TEST(grants(BuiltinRoles::Reader, permission) == reads, permission + " is on the wrong side of the reader rule");
    }
}

// The rule above is stated twice on purpose - once here and once in BuiltinRoles.cpp - so it says
// what it is checking rather than deferring to the thing under test. The consequence is that a
// module gaining an action whose name does not fit euclid's read/write naming shows up here as a
// failure, which is the moment to decide whether the action is misnamed or the rule is too narrow.
BOOST_AUTO_TEST_CASE(CountingIsReading) {

    // The case that widened it: euclid names the cached figures get-object-count and
    // get-message-count, which the get- prefix already covered, and the one action that counts for
    // real count-objects, which it did not. A reader able to list a bucket's objects but not be
    // told how many there are would be a strange thing to have.
    BOOST_TEST(grants(BuiltinRoles::Reader, "esm:count-objects"));
    BOOST_TEST(grants(BuiltinRoles::Reader, "esm:get-object-count"));
    BOOST_TEST(grants(BuiltinRoles::Reader, "esm:list-objects"));

    // And it is still only reading: counting does not carry the thing counted.
    BOOST_TEST(!grants(BuiltinRoles::Reader, "esm:put-object"));
    BOOST_TEST(!grants(BuiltinRoles::Reader, "esm:delete-object"));
}

// Worth stating: reader includes eam:list-users, which is a read but is also who-can-see-whom. It
// is deliberate - a reader role that hid the user list would still show every queue and object -
// but it is the entry somebody will ask about.
BOOST_AUTO_TEST_CASE(ReaderIncludesAccessManagementReads) {

    BOOST_TEST(grants(BuiltinRoles::Reader, "eam:list-users"));
    BOOST_TEST(!grants(BuiltinRoles::Reader, "eam:delete-user"));
}

// ── publisher, consumer, application ────────────────────────────────────────

BOOST_AUTO_TEST_CASE(PublisherPublishesAndNothingElse) {

    BOOST_TEST(grants(BuiltinRoles::Publisher, "ens:publish-message"));
    BOOST_TEST(grants(BuiltinRoles::Publisher, "eqs:send-message"));
    // It has to be able to resolve a name, or it can only address what it was told the ERN of.
    BOOST_TEST(grants(BuiltinRoles::Publisher, "ens:get-topic-ern"));
    BOOST_TEST(grants(BuiltinRoles::Publisher, "eqs:get-queue-ern"));

    BOOST_TEST(!grants(BuiltinRoles::Publisher, "eqs:receive-messages"));
    BOOST_TEST(!grants(BuiltinRoles::Publisher, "ens:create-topic"));
    BOOST_TEST(!grants(BuiltinRoles::Publisher, "ens:delete-topic"));
}

BOOST_AUTO_TEST_CASE(ConsumerConsumesAndNothingElse) {

    BOOST_TEST(grants(BuiltinRoles::Consumer, "eqs:receive-messages"));
    BOOST_TEST(grants(BuiltinRoles::Consumer, "eqs:delete-message"));
    BOOST_TEST(grants(BuiltinRoles::Consumer, "ens:subscribe"));
    BOOST_TEST(grants(BuiltinRoles::Consumer, "ens:unsubscribe"));

    BOOST_TEST(!grants(BuiltinRoles::Consumer, "eqs:send-message"));
    BOOST_TEST(!grants(BuiltinRoles::Consumer, "eqs:delete-queue"));
    BOOST_TEST(!grants(BuiltinRoles::Consumer, "ens:publish-message"));
}

// EQS dispatches one command under two names. A consumer that reached only one of them would work
// or not depending on which spelling its client happened to send.
BOOST_AUTO_TEST_CASE(ConsumerReachesBothSpellingsOfSetVisibility) {

    BOOST_TEST(grants(BuiltinRoles::Consumer, "eqs:set-visibility"));
    BOOST_TEST(grants(BuiltinRoles::Consumer, "eqs:set-message-visibility"));
}

BOOST_AUTO_TEST_CASE(ApplicationIsPublisherAndConsumerPlusObjects) {

    for (const auto &permission: BuiltinRoles::PermissionsOf(BuiltinRoles::Publisher)) {
        BOOST_TEST(grants(BuiltinRoles::Application, permission), "application does not grant publisher's " + permission);
    }
    for (const auto &permission: BuiltinRoles::PermissionsOf(BuiltinRoles::Consumer)) {
        BOOST_TEST(grants(BuiltinRoles::Application, permission), "application does not grant consumer's " + permission);
    }

    BOOST_TEST(grants(BuiltinRoles::Application, "esm:get-object"));
    BOOST_TEST(grants(BuiltinRoles::Application, "esm:put-object"));

    BOOST_TEST(!grants(BuiltinRoles::Application, "esm:delete-bucket"));
    BOOST_TEST(!grants(BuiltinRoles::Application, "esm:create-bucket"));
    BOOST_TEST(!grants(BuiltinRoles::Application, "eam:register"));
}

// An application does not receive from a topic or a bucket - it receives from a queue of its own
// that it subscribes to one, so that every instance gets the message rather than whichever asked
// first. The queue is therefore part of the application and its whole life is the application's to
// manage. euclid-spring's listener container does exactly this on startup and shutdown, and throws
// if it cannot, so an application missing any of these does not start at all.
BOOST_AUTO_TEST_CASE(ApplicationOwnsTheDeliveryQueueItConsumesThrough) {

    BOOST_TEST(grants(BuiltinRoles::Application, "eqs:create-queue"));
    BOOST_TEST(grants(BuiltinRoles::Application, "eqs:delete-queue"));

    // Not for browsing: this is how a restart finds the queues a run that was killed rather than
    // stopped left behind, which is the only way they are ever cleaned up.
    BOOST_TEST(grants(BuiltinRoles::Application, "eqs:list-queues"));

    // And the subscriptions that feed it, on both sides. The list- actions are what make a restart
    // idempotent: a listener checks whether it is already subscribed instead of subscribing twice.
    BOOST_TEST(grants(BuiltinRoles::Application, "ens:subscribe"));
    BOOST_TEST(grants(BuiltinRoles::Application, "ens:unsubscribe"));
    BOOST_TEST(grants(BuiltinRoles::Application, "ens:list-subscriptions"));
    BOOST_TEST(grants(BuiltinRoles::Application, "esm:subscribe"));
    BOOST_TEST(grants(BuiltinRoles::Application, "esm:unsubscribe"));
    BOOST_TEST(grants(BuiltinRoles::Application, "esm:list-subscriptions"));
}

// Owning a queue is not owning the thing it is fed from. An application may take down its own
// delivery queue; it may not take down the topic other applications are also listening to, nor the
// bucket whose events it subscribed to.
BOOST_AUTO_TEST_CASE(ApplicationDoesNotReachWhatItSubscribesTo) {

    BOOST_TEST(!grants(BuiltinRoles::Application, "ens:create-topic"));
    BOOST_TEST(!grants(BuiltinRoles::Application, "ens:delete-topic"));
    BOOST_TEST(!grants(BuiltinRoles::Application, "ens:purge-topic"));
    BOOST_TEST(!grants(BuiltinRoles::Application, "esm:delete-bucket"));
    BOOST_TEST(!grants(BuiltinRoles::Application, "esm:purge-bucket"));

    // Nor the queue's contents wholesale - a consumer deletes the messages it has handled.
    BOOST_TEST(!grants(BuiltinRoles::Application, "eqs:purge-queue"));
}

// ── transfer ────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(TransferGrantsEveryTransferCommandAndNoServerAdministration) {

    BOOST_TEST(grants(BuiltinRoles::Transfer, "ets:list-directory"));
    BOOST_TEST(grants(BuiltinRoles::Transfer, "ets:get-file"));
    BOOST_TEST(grants(BuiltinRoles::Transfer, "ets:put-file"));
    BOOST_TEST(grants(BuiltinRoles::Transfer, "ets:rename-file"));
    BOOST_TEST(grants(BuiltinRoles::Transfer, "ets:delete-file"));
    BOOST_TEST(grants(BuiltinRoles::Transfer, "ets:create-directory"));
    BOOST_TEST(grants(BuiltinRoles::Transfer, "ets:delete-directory"));

    // The line this role exists on: a client that may upload must not be able to stop the server
    // it uploads to, and both are ets:.
    BOOST_TEST(!grants(BuiltinRoles::Transfer, "ets:stop-server"));
    BOOST_TEST(!grants(BuiltinRoles::Transfer, "ets:start-server"));
    BOOST_TEST(!grants(BuiltinRoles::Transfer, "ets:create-server"));
    BOOST_TEST(!grants(BuiltinRoles::Transfer, "ets:delete-server"));
    BOOST_TEST(!grants(BuiltinRoles::Transfer, "ets:update-server"));

    BOOST_TEST(!grants(BuiltinRoles::Transfer, "eam:register"));
    BOOST_TEST(!grants(BuiltinRoles::Transfer, "eqs:send-message"));
}

// The half that is easy to leave out and impossible to notice from the ets: side alone: a transfer
// server stores nothing itself, so every command it allows turns into an ESM call made with the
// client's own token. A role granting the FTP verb and not the storage action passes the FTP check
// and is refused one layer down.
BOOST_AUTO_TEST_CASE(TransferReachesTheBucketItsCommandsGoThrough) {

    // Exactly what Transfer::TransferStorage calls.
    BOOST_TEST(grants(BuiltinRoles::Transfer, "esm:list-objects"));
    BOOST_TEST(grants(BuiltinRoles::Transfer, "esm:get-object"));
    BOOST_TEST(grants(BuiltinRoles::Transfer, "esm:put-object"));
    BOOST_TEST(grants(BuiltinRoles::Transfer, "esm:delete-object"));

    // And no more of ESM than that. A transfer client works inside a bucket somebody else made
    // for it, and must not be able to make, rename or remove one.
    BOOST_TEST(!grants(BuiltinRoles::Transfer, "esm:create-bucket"));
    BOOST_TEST(!grants(BuiltinRoles::Transfer, "esm:delete-bucket"));
    BOOST_TEST(!grants(BuiltinRoles::Transfer, "esm:rename-bucket"));
    BOOST_TEST(!grants(BuiltinRoles::Transfer, "esm:purge-bucket"));
    BOOST_TEST(!grants(BuiltinRoles::Transfer, "esm:subscribe"));
}

// The computed roles have to cover the transfer commands too, or an installation whose users hold
// `operator` finds its FTP clients refused after an upgrade - which is the whole migration this
// was meant to avoid making painful.
BOOST_AUTO_TEST_CASE(ReaderAndOperatorReachTheTransferCommandsTheyShould) {

    // reader reads: list and download, by the get-/list- rule.
    BOOST_TEST(grants(BuiltinRoles::Reader, "ets:list-directory"));
    BOOST_TEST(grants(BuiltinRoles::Reader, "ets:get-file"));
    BOOST_TEST(!grants(BuiltinRoles::Reader, "ets:put-file"));
    BOOST_TEST(!grants(BuiltinRoles::Reader, "ets:delete-file"));

    // operator does everything that is not destructive.
    BOOST_TEST(grants(BuiltinRoles::Operator, "ets:put-file"));
    BOOST_TEST(grants(BuiltinRoles::Operator, "ets:create-directory"));
    BOOST_TEST(grants(BuiltinRoles::Operator, "ets:rename-file"));
    BOOST_TEST(!grants(BuiltinRoles::Operator, "ets:delete-file"));
    BOOST_TEST(!grants(BuiltinRoles::Operator, "ets:delete-directory"));
}

BOOST_AUTO_TEST_CASE(NoBuiltinRoleGrantsDuplicatePermissions) {

    for (const auto &role: BuiltinRoles::Names()) {
        const auto &permissions = BuiltinRoles::PermissionsOf(role);
        BOOST_TEST(std::ranges::is_sorted(permissions), "built-in role '" + role + "' is not sorted");

        const bool duplicated = std::ranges::adjacent_find(permissions) != permissions.end();
        BOOST_TEST(!duplicated, "built-in role '" + role + "' names a permission twice");
    }
}
