// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE ArtifactFetchTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <string>

// Euclid includes
#include <euclid/core/ArtifactFetcher.h>
#include <euclid/core/ModuleClient.h>
#include <euclid/core/Permissions.h>

namespace http = boost::beast::http;

using Euclid::Core::Artifact::Detail::FitsInOneCall;
using Euclid::Core::Artifact::Detail::PartCount;

// The manager used to read an application's artifact straight off ESM's data directory. A worker
// has no such directory and no database, so that could never be the path a worker takes - and two
// paths would mean the one that matters is the one nobody exercises. It downloads through ESM now,
// on the host where a failure is easy to look at. See docs/worker-nodes.md §3.1.
//
// Two rules decide whether the bytes that arrive are the object, and both fail quietly when wrong,
// which is why they are separated from the socket work and pinned here.

// ── Which path a size takes ─────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(ASmallObjectGoesInOneCall) {

    BOOST_TEST(FitsInOneCall(1, 8 * 1024 * 1024));
    BOOST_TEST(FitsInOneCall(8 * 1024 * 1024 - 1, 8 * 1024 * 1024));
}

BOOST_AUTO_TEST_CASE(AnObjectExactlyThePartSizeDoesNot) {

    // The boundary, and the one worth having a test for. EsmServer::handleGetObject answers 413
    // when the object's size is *at or above* the part size the request names, so this has to be
    // strictly less-than. A <= would send an artifact whose size lands exactly on the part size
    // down the single-shot path and have it refused - a deploy that fails for one file size only,
    // which is not a pattern anybody goes looking for.
    BOOST_TEST(!FitsInOneCall(8 * 1024 * 1024, 8 * 1024 * 1024));
}

BOOST_AUTO_TEST_CASE(ALargeObjectGoesInParts) {

    BOOST_TEST(!FitsInOneCall(8 * 1024 * 1024 + 1, 8 * 1024 * 1024));
    BOOST_TEST(!FitsInOneCall(300L * 1024 * 1024, 8 * 1024 * 1024));
}

// ── How many parts ──────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(APartialLastPartIsStillAPart) {

    // The ceiling, which is the whole point. A floor does not fail - it writes a truncated
    // artifact, renames it into place, and leaves a jar that is almost right. The md5 check would
    // notice on the next pass and fetch it again, equally truncated, for ever.
    BOOST_TEST(PartCount(10, 4) == 3L);
    BOOST_TEST(PartCount(9, 4) == 3L);
    BOOST_TEST(PartCount(5, 4) == 2L);
}

BOOST_AUTO_TEST_CASE(AnExactMultipleIsNotRoundedUp) {

    BOOST_TEST(PartCount(8, 4) == 2L);
    BOOST_TEST(PartCount(4, 4) == 1L);
}

BOOST_AUTO_TEST_CASE(EveryByteIsCoveredBySomePart) {

    // The property the two cases above are examples of, over sizes either side of each boundary:
    // the parts have to cover the object, and must not add a part that has nothing in it - an
    // empty download-part would be requested beyond the end of the object and refused.
    constexpr long partSize = 8;
    for (long size = 1; size <= 64; ++size) {
        const auto parts = PartCount(size, partSize);
        BOOST_TEST(parts * partSize >= size, "size " + std::to_string(size) + " is not fully covered");
        BOOST_TEST((parts - 1) * partSize < size, "size " + std::to_string(size) + " asks for a part with nothing in it");
    }
}

BOOST_AUTO_TEST_CASE(AnEmptyObjectAsksForNoParts) {

    // An artifact of zero bytes is not something anybody deploys on purpose, but create-download
    // answers size 0 for one and the loop must not then ask for part 1 of 0.
    BOOST_TEST(PartCount(0, 8) == 0L);
    BOOST_TEST(PartCount(-1, 8) == 0L);
}

BOOST_AUTO_TEST_CASE(AnUnusablePartSizeAsksForNoParts) {

    // PartSize() floors the configured value at 1, so this is unreachable from configuration -
    // held anyway because the alternative is a division by zero rather than a wrong answer.
    BOOST_TEST(PartCount(100, 0) == 0L);
}

// ── The calls it makes ──────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(TheActionsItUsesArePermissionsThatExist) {

    // The manager reaches ESM over a module socket, so it builds the request itself and the
    // authorization gate requires "<target>:<action>". An action named here that is not in the
    // vocabulary is refused before a grant is read - by every role, including one granted
    // everything - which is how the transfer servers once took a 403 on every call.
    for (const auto &action: {"get-object", "create-download", "download-part", "complete-download"}) {
        BOOST_TEST(Euclid::Core::Permissions::Exists(Euclid::Core::Permissions::Of("esm", action)),
                   std::string("esm:") + action + " is not a permission any role can hold");
    }
}

BOOST_AUTO_TEST_CASE(TheRequestCarriesBothHalvesOfThePermission) {

    // The transport moved from the transfer servers' library into core when the manager came to
    // need it; this is the half of the contract the manager now depends on too.
    const auto request = Euclid::Core::ModuleClient::BuildRequest(
            "esm", "get-object", "token-value",
            {{"x-euclid-bucket-ern", "ern:esm:...:bucket:apps"}, {"x-euclid-key", "billing.jar"}}, "");

    BOOST_TEST(std::string(request["x-euclid-target"]) == "esm");
    BOOST_TEST(std::string(request["x-euclid-action"]) == "get-object");
    BOOST_TEST(std::string(request[http::field::authorization]) == "Bearer token-value");
    BOOST_TEST(std::string(request["x-euclid-bucket-ern"]) == "ern:esm:...:bucket:apps");
    BOOST_TEST(std::string(request["x-euclid-key"]) == "billing.jar");
}
