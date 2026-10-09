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
    BOOST_TEST(grants(BuiltinRoles::Reader, "ekv:get-table"));

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
// An application behind EAG treats euclid as its identity provider: the gateway states who the
// caller is in x-euclid-user-id, and the application maps that caller's euclid groups onto its own
// roles. Without the lookup it cannot authorise anybody, so every request through the gateway is
// answered 401 by the application - a failure that reads as an authentication problem and is not
// one.
BOOST_AUTO_TEST_CASE(ApplicationCanResolveTheRolesOfItsCaller) {

    BOOST_TEST(grants(BuiltinRoles::Application, "eam:list-user-groups"));

    // Reading, and nothing more. An application that can see which groups exist must not be able to
    // put itself in one, invent one, or read the users behind them - which is the difference
    // between resolving a caller's roles and administering the installation.
    BOOST_TEST(!grants(BuiltinRoles::Application, "eam:create-user-group"));
    BOOST_TEST(!grants(BuiltinRoles::Application, "eam:delete-user-group"));
    BOOST_TEST(!grants(BuiltinRoles::Application, "eam:user-group-add-user"));
    BOOST_TEST(!grants(BuiltinRoles::Application, "eam:list-users"));
}

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

// An application serves no gateway request, so acquireInstance() never marks it busy and the only
// thing the autoscaler can learn about it is what it says about itself. Saying it is a call like any
// other, and a call the role does not hold is refused - so leaving this out does not degrade
// scaling, it removes it: the report is answered 403, the manager sees nothing, and the pool stays
// at one instance however much work is waiting. Which is exactly what happened.
BOOST_AUTO_TEST_CASE(ApplicationMayReportItsOwnLoad) {

    BOOST_TEST(grants(BuiltinRoles::Application, "eap:report-load"));

    // And its own metrics, which is the other half and not the same half. EMO answers "what has
    // this application been doing for the last fortnight" and answers it a bucket at a time;
    // report-load answers "how busy is it right now" and the manager reads it on the next
    // reconcile. An application that holds only the first drives the autoscaler minutes late, and
    // one that holds neither is invisible to both - which is what a 403 on every push made of it.
    BOOST_TEST(grants(BuiltinRoles::Application, "emo:push-metrics"));

    // The backlog half of the report, and the half that actually grows a listener's pool:
    // utilisation is measured against the sum of every listener's concurrency, so an instance whose
    // one busy queue is saturated still reads as a fraction loaded and never reaches the saturation
    // bar. Left out, this fails differently from report-load above - the container counts
    // best-effort, so the refusal goes to debug and the report still arrives saying backlog 0,
    // which is indistinguishable from an empty queue.
    BOOST_TEST(grants(BuiltinRoles::Application, "eqs:get-message-count"));

    // Reading the monitoring store back is not part of it. An application reports about itself; it
    // does not get to see what the installation has been doing.
    BOOST_TEST(!grants(BuiltinRoles::Application, "emo:list"));
    BOOST_TEST(!grants(BuiltinRoles::Application, "emo:average"));

    // And nothing else of EAP. An application deploys nothing, starts nothing and stops nothing -
    // least of all itself.
    BOOST_TEST(!grants(BuiltinRoles::Application, "eap:create-application"));
    BOOST_TEST(!grants(BuiltinRoles::Application, "eap:delete-application"));
    BOOST_TEST(!grants(BuiltinRoles::Application, "eap:stop-application"));
    BOOST_TEST(!grants(BuiltinRoles::Application, "eap:redeploy-application"));
    BOOST_TEST(!grants(BuiltinRoles::Application, "eap:set-log-level"));
}

// ── node ────────────────────────────────────────────────────────────────────

// What euclid-wrk calls as itself. Missing one of these is not a degraded worker, it is one that does
// not work: no registration, no renewal, an instance it cannot give credentials to, or an artifact it
// can fetch only while it is under the part size.
BOOST_AUTO_TEST_CASE(NodeHoldsWhatAWorkerCallsAsItself) {

    BOOST_TEST(grants(BuiltinRoles::Node, "eap:register-node"));
    BOOST_TEST(grants(BuiltinRoles::Node, "eap:renew-node"));
    BOOST_TEST(grants(BuiltinRoles::Node, "eap:issue-instance-credentials"));
    BOOST_TEST(grants(BuiltinRoles::Node, "eap:report-node-instance"));

    // The single-shot fetch and the three of a fetch in parts.
    BOOST_TEST(grants(BuiltinRoles::Node, "esm:get-object"));
    BOOST_TEST(grants(BuiltinRoles::Node, "esm:create-download"));
    BOOST_TEST(grants(BuiltinRoles::Node, "esm:download-part"));
    BOOST_TEST(grants(BuiltinRoles::Node, "esm:complete-download"));
}

// And nothing an operator does to a node, nor anything an application does. A node that could free
// another node's name could take over its assignments and the credentials that go with them.
BOOST_AUTO_TEST_CASE(NodeCannotManageNodesOrApplications) {

    BOOST_TEST(!grants(BuiltinRoles::Node, "eap:delete-node"));
    BOOST_TEST(!grants(BuiltinRoles::Node, "eap:drain-node"));
    BOOST_TEST(!grants(BuiltinRoles::Node, "eap:assign-instance"));
    BOOST_TEST(!grants(BuiltinRoles::Node, "eap:create-application"));
    BOOST_TEST(!grants(BuiltinRoles::Node, "esm:put-object"));
    BOOST_TEST(!grants(BuiltinRoles::Node, "esm:delete-object"));
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

// An application reads the credentials of the database it talks to while its Spring context is
// being built, which is the first thing it does and the reason ESS exists. Withholding this made
// ESS unreachable by anything deployed - no role an application could hold named a single ess
// permission - so every read was answered 403 before ESS saw the request and the application
// terminated in the datasource it was trying to construct.
BOOST_AUTO_TEST_CASE(ApplicationReadsTheSecretsItIsConfiguredWith) {

    BOOST_TEST(grants(BuiltinRoles::Application, "ess:get-secret"));

    // Reading, and nothing else. A principal that can rotate or destroy the credentials it uses is
    // a different thing from one that uses them, and the manifest's secrets are declared by the
    // application precisely so that an operator decides what the value is.
    BOOST_TEST(!grants(BuiltinRoles::Application, "ess:create-secret"));
    BOOST_TEST(!grants(BuiltinRoles::Application, "ess:update-secret"));
    BOOST_TEST(!grants(BuiltinRoles::Application, "ess:delete-secret"));
    BOOST_TEST(!grants(BuiltinRoles::Application, "ess:add-secret-tag"));
    BOOST_TEST(!grants(BuiltinRoles::Application, "ess:delete-secret-tag"));

    // Deliberately not the listing. An application needs the value of a secret whose name its
    // deployment configured; the names of the ones it was not told about are reconnaissance, and
    // ESS answers list-secrets by filtering rather than refusing - so holding it would turn a
    // narrow grant into a map of everything the grant does not cover.
    BOOST_TEST(!grants(BuiltinRoles::Application, "ess:list-secrets"));
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

// A file too large to move in one call takes the multipart path, and TransferStorage picks that
// path by size rather than by permission. Holding only the single-shot pair meant a transfer user
// could store a small file and not a large one - and the refusal arrives at CLOSE, after every byte
// has been sent, as "Could not store object" with nothing to say which path it was on.
BOOST_AUTO_TEST_CASE(TransferCanMoveAFileTooBigForOneCall) {

    // Upload: what UploadStream calls above the part size.
    BOOST_TEST(grants(BuiltinRoles::Transfer, "esm:create-upload"));
    BOOST_TEST(grants(BuiltinRoles::Transfer, "esm:upload-part"));
    BOOST_TEST(grants(BuiltinRoles::Transfer, "esm:complete-upload"));

    // Download: the same, in the other direction. Both halves, because a user who can put a large
    // file and not get it back is as broken as one who cannot put it.
    BOOST_TEST(grants(BuiltinRoles::Transfer, "esm:create-download"));
    BOOST_TEST(grants(BuiltinRoles::Transfer, "esm:download-part"));
    BOOST_TEST(grants(BuiltinRoles::Transfer, "esm:complete-download"));

    // Still inside somebody else's bucket: moving bytes in pieces is not permission to make or
    // destroy the thing they are moved into.
    BOOST_TEST(!grants(BuiltinRoles::Transfer, "esm:create-bucket"));
    BOOST_TEST(!grants(BuiltinRoles::Transfer, "esm:delete-bucket"));
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
