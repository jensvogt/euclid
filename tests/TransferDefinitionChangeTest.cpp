// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE TransferDefinitionChangeTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <chrono>
#include <string>
#include <vector>

// Euclid includes
#include <euclid/database/entity/ets/TransferServer.h>

using Euclid::Database::Entity::ETS::TransferProtocol;
using Euclid::Database::Entity::ETS::TransferServer;
using Euclid::Database::Entity::ETS::TransferServerState;

// A euclid-ftp or euclid-sftp process reads its definition once, as it comes up, and nothing tells
// it later. So `ets update-server` on a running server used to appear to take and silently do
// nothing - no directories created, no new user admitted - until somebody restarted it by hand.
// The manager now compares this fingerprint against what the running process was started with.
//
// Both directions matter and both are cheap to get wrong. Too insensitive and the original bug is
// back, invisibly. Too sensitive and every reconcile tick restarts a live server, dropping the
// transfers in flight - which is why the timestamps are excluded, `modified` being rewritten by an
// update that changed nothing at all.

namespace {

    TransferServer serverOf() {
        TransferServer server;
        server.serverId = "incoming";
        server.runtimeName = "incoming";
        server.ern = "ern:ets:eu-central-1:000000000000:production:server:incoming";
        server.accountId = "000000000000";
        server.nameSpace = "production";
        server.region = "eu-central-1";
        server.protocol = TransferProtocol::FTP;
        server.address = "0.0.0.0";
        server.port = 2121;
        server.bucketErn = "ern:esm:eu-central-1:000000000000:production:bucket:dropbox";
        server.bucketName = "dropbox";
        server.homeDirectory = "{user}";
        server.userIds = {"jvo"};
        server.userGroups = {"suppliers"};
        server.directories = {"incoming/mix", "feedback"};
        server.desiredState = TransferServerState::RUNNING;
        server.pasvMin = 6000;
        server.pasvMax = 6020;
        server.created = std::chrono::system_clock::now();
        server.modified = server.created;
        return server;
    }

}// namespace

BOOST_AUTO_TEST_CASE(AnUnchangedDefinitionFingerprintsTheSame) {

    // Otherwise every reconcile tick would restart every transfer server.
    BOOST_TEST(serverOf().runtimeFingerprint() == serverOf().runtimeFingerprint());
}

// ── What must NOT restart a running server ──────────────────────────────────

BOOST_AUTO_TEST_CASE(TheModifiedTimestampDoesNotCount) {

    // The one that would bite hardest: update-server rewrites `modified` whether or not anything
    // else changed, so counting it would turn a no-op edit into dropped transfers.
    auto server = serverOf();
    const auto before = server.runtimeFingerprint();

    server.modified = server.modified + std::chrono::hours(3);
    BOOST_TEST(server.runtimeFingerprint() == before);
}

BOOST_AUTO_TEST_CASE(TheCreatedTimestampDoesNotCount) {

    auto server = serverOf();
    const auto before = server.runtimeFingerprint();

    server.created = server.created - std::chrono::hours(48);
    BOOST_TEST(server.runtimeFingerprint() == before);
}

BOOST_AUTO_TEST_CASE(StartingAndStoppingDoesNotCount) {

    // desiredState is what the reconciler already acts on, one branch earlier. Counting it here
    // would make a server restart itself immediately after being started.
    auto server = serverOf();
    const auto before = server.runtimeFingerprint();

    server.desiredState = TransferServerState::STOPPED;
    BOOST_TEST(server.runtimeFingerprint() == before);
}

// ── What must restart a running server ──────────────────────────────────────

BOOST_AUTO_TEST_CASE(TheDirectoriesCount) {

    // The one that started this: directories are created at login from the definition the process
    // read at startup, so adding one to a running server did nothing at all.
    auto server = serverOf();
    const auto before = server.runtimeFingerprint();

    server.directories.emplace_back("outgoing");
    BOOST_TEST(server.runtimeFingerprint() != before);
}

BOOST_AUTO_TEST_CASE(WhoMayLogInCounts) {

    // TransferAuthenticator is constructed per login from the same startup snapshot, so a user
    // added to a running server could not log in until it was restarted.
    auto server = serverOf();
    const auto beforeUsers = server.runtimeFingerprint();
    server.userIds.emplace_back("acme");
    BOOST_TEST(server.runtimeFingerprint() != beforeUsers);

    const auto beforeGroups = server.runtimeFingerprint();
    server.userGroups.emplace_back("partners");
    BOOST_TEST(server.runtimeFingerprint() != beforeGroups);
}

BOOST_AUTO_TEST_CASE(TheHomeDirectoryCounts) {

    auto server = serverOf();
    const auto before = server.runtimeFingerprint();

    server.homeDirectory = "suppliers/{user}";
    BOOST_TEST(server.runtimeFingerprint() != before);
}

BOOST_AUTO_TEST_CASE(TheBucketCounts) {

    auto server = serverOf();
    const auto before = server.runtimeFingerprint();

    server.bucketErn = "ern:esm:eu-central-1:000000000000:production:bucket:elsewhere";
    BOOST_TEST(server.runtimeFingerprint() != before);
}

BOOST_AUTO_TEST_CASE(WhereItListensCounts) {

    auto server = serverOf();

    const auto beforePort = server.runtimeFingerprint();
    server.port = 2222;
    BOOST_TEST(server.runtimeFingerprint() != beforePort);

    const auto beforeAddress = server.runtimeFingerprint();
    server.address = "10.0.0.5";
    BOOST_TEST(server.runtimeFingerprint() != beforeAddress);

    const auto beforePasv = server.runtimeFingerprint();
    server.pasvMax = 6100;
    BOOST_TEST(server.runtimeFingerprint() != beforePasv);
}

BOOST_AUTO_TEST_CASE(TheScopeTheAuthorizerUsesCounts) {

    // The server's account, namespace and ERN are what a grant is matched against - see
    // Transfer::TransferAuthorizer - so a definition moved between namespaces changes who may do
    // what on it.
    auto server = serverOf();

    const auto beforeNamespace = server.runtimeFingerprint();
    server.nameSpace = "staging";
    BOOST_TEST(server.runtimeFingerprint() != beforeNamespace);

    const auto beforeErn = server.runtimeFingerprint();
    server.ern = "ern:ets:eu-central-1:000000000000:staging:server:incoming";
    BOOST_TEST(server.runtimeFingerprint() != beforeErn);
}
