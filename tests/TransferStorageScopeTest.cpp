// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE TransferStorageScopeTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

// Euclid includes
#include <TransferStorage.h>

using Euclid::Transfer::TransferStorage;

// Every ESM call a transfer server makes is made as the client who logged in, over a module socket,
// with the scope carried in headers rather than in a session. Those headers are the whole of what
// the far end knows about where the call belongs, and getting one wrong fails in ways no FTP client
// reports.
//
// The namespace was missing from them until 2026-09-13, and it cost two things at once, both
// silent. An object stored through a transfer server was recorded with no namespace, so the
// esm.object.created event it raised carried an empty one and a subscriber filtering on namespace
// never matched it - a file uploaded by FTP simply never reached whatever was waiting for it. And
// the authorization gate was asked about namespace "" while the transfer server's own check asked
// about the server's real namespace, so a role granted in a namespace passed the FTP command and
// was refused on the storage call behind it.

namespace {

    using Headers = std::vector<std::pair<std::string, std::string>>;

    TransferStorage storageOf(const std::string &region = "eu-central-1",
                              const std::string &accountId = "000000000000",
                              const std::string &nameSpace = "production") {
        return TransferStorage{"ern:esm:eu-central-1:000000000000:production:bucket:dropbox",
                               "a-bearer-token", region, accountId, nameSpace, "incoming", "jvo"};
    }

    bool sends(const Headers &headers, const std::string &name, const std::string &value) {
        return std::ranges::any_of(headers, [&](const auto &header) {
            return header.first == name && header.second == value;
        });
    }

    bool mentions(const Headers &headers, const std::string &name) {
        return std::ranges::any_of(headers, [&](const auto &header) { return header.first == name; });
    }

}// namespace

BOOST_AUTO_TEST_CASE(EveryCallCarriesRegionAccountAndNamespace) {

    const auto headers = storageOf().scopedHeaders({});

    BOOST_TEST(sends(headers, "x-euclid-region", "eu-central-1"));
    BOOST_TEST(sends(headers, "x-euclid-account-id", "000000000000"));
    BOOST_TEST(sends(headers, "x-euclid-namespace", "production"));
}

BOOST_AUTO_TEST_CASE(TheNamespaceIsTheTransferServersOwn) {

    // Not the bucket's, not the user's home prefix: the grant that authorizes the storage call is
    // scoped by the same namespace the transfer server's own permission check uses, so the two
    // have to name it identically or one half of the `transfer` role refuses what the other
    // allowed.
    const auto headers = storageOf("eu-central-1", "000000000000", "staging").scopedHeaders({});

    BOOST_TEST(sends(headers, "x-euclid-namespace", "staging"));
}

BOOST_AUTO_TEST_CASE(TheCallsOwnHeadersAreKept) {

    // The scope is added to what the caller asked for, never instead of it - a put-object that
    // lost its bucket or its key would be a very confusing 400.
    const auto headers = storageOf().scopedHeaders({{"x-euclid-bucket-ern", "ern:esm:...:bucket:dropbox"},
                                                    {"x-euclid-key", "jvo/incoming/mix/file.xml"}});

    BOOST_TEST(sends(headers, "x-euclid-bucket-ern", "ern:esm:...:bucket:dropbox"));
    BOOST_TEST(sends(headers, "x-euclid-key", "jvo/incoming/mix/file.xml"));
    BOOST_TEST(sends(headers, "x-euclid-namespace", "production"));
    BOOST_TEST(headers.size() == 5U);
}

BOOST_AUTO_TEST_CASE(AnUnsetScopeFieldIsLeftOutRatherThanSentEmpty) {

    // A server at the bucket root of an installation that configures no region: an empty header
    // value reads to the module exactly like an absent one, so sending it buys nothing and makes
    // a missing value look deliberate in a packet capture.
    const auto headers = storageOf("", "", "").scopedHeaders({});

    BOOST_TEST(!mentions(headers, "x-euclid-region"));
    BOOST_TEST(!mentions(headers, "x-euclid-account-id"));
    BOOST_TEST(!mentions(headers, "x-euclid-namespace"));
    BOOST_TEST(headers.empty());
}
