// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE UploadTargetTest
#include <boost/test/unit_test.hpp>

// Euclid includes
#include <UploadTarget.h>

using Euclid::EAG::ResolveUploadKey;
using Route = Euclid::Database::Entity::EAG::Route;

// The key an upload writes to comes from the caller's own URL, which makes this the one place in
// the upload path where a caller chooses something the server then uses as a name. The route's
// keyPrefix is what confines it - a transfer server's home directory, applied to keys - so what
// matters here is that nothing a caller can write climbs back out of it.

namespace {

    Route route(const std::string &path = "/upload", const std::string &keyPrefix = {}) {
        Route r;
        r.path = path;
        r.upload.keyPrefix = keyPrefix;
        return r;
    }

}// namespace

// ── The ordinary case ───────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(TheKeyIsThePathBeneathTheRoute) {
    const auto resolved = ResolveUploadKey(route(), "/upload/onix/2026-09.xml");

    BOOST_TEST(resolved.valid);
    BOOST_TEST(resolved.key == "onix/2026-09.xml");
}

BOOST_AUTO_TEST_CASE(ThePrefixGoesInFront) {
    const auto resolved = ResolveUploadKey(route("/upload", "suppliers/jvo"), "/upload/2026-09.xml");

    BOOST_TEST(resolved.valid);
    BOOST_TEST(resolved.key == "suppliers/jvo/2026-09.xml");
}

// Whether the prefix was written with a trailing slash is not something a caller should be able to
// tell from the keys that come out.
BOOST_AUTO_TEST_CASE(APrefixJoinsTheSameWayWithOrWithoutItsSlash) {
    BOOST_TEST(ResolveUploadKey(route("/upload", "a/b"), "/upload/x.xml").key == "a/b/x.xml");
    BOOST_TEST(ResolveUploadKey(route("/upload", "a/b/"), "/upload/x.xml").key == "a/b/x.xml");
    BOOST_TEST(ResolveUploadKey(route("/upload", "/a/b"), "/upload/x.xml").key == "a/b/x.xml");
}

BOOST_AUTO_TEST_CASE(AQueryStringIsNotPartOfTheKey) {
    BOOST_TEST(ResolveUploadKey(route(), "/upload/x.xml?overwrite=true").key == "x.xml");
}

BOOST_AUTO_TEST_CASE(AnEscapedCharacterBecomesTheCharacter) {
    BOOST_TEST(ResolveUploadKey(route(), "/upload/two%20words.xml").key == "two words.xml");
    BOOST_TEST(ResolveUploadKey(route(), "/upload/a%2Bb.xml").key == "a+b.xml");
}

// "+" is a space in a query string and a plain plus in a path, and this only reads paths.
BOOST_AUTO_TEST_CASE(APlusStaysAPlus) {
    BOOST_TEST(ResolveUploadKey(route(), "/upload/a+b.xml").key == "a+b.xml");
}

// ── Climbing out ────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(ADotDotSegmentIsRefused) {
    const auto resolved = ResolveUploadKey(route("/upload", "suppliers/jvo"), "/upload/../../etc/passwd");

    BOOST_TEST(!resolved.valid);
    BOOST_TEST(!resolved.reason.empty());
}

// The check is made after decoding, because a caller who writes "%2E%2E" means ".." and a check
// made before would not have seen it.
BOOST_AUTO_TEST_CASE(AnEscapedDotDotIsRefusedToo) {
    BOOST_TEST(!ResolveUploadKey(route(), "/upload/%2E%2E/%2E%2E/etc/passwd").valid);
    BOOST_TEST(!ResolveUploadKey(route(), "/upload/a/%2e%2e/b").valid);
}

BOOST_AUTO_TEST_CASE(ASingleDotSegmentIsRefused) {
    BOOST_TEST(!ResolveUploadKey(route(), "/upload/./x.xml").valid);
    BOOST_TEST(!ResolveUploadKey(route(), "/upload/a/./b").valid);
}

// Refused rather than collapsed: a caller who wrote "a//b" and got "a/b" has been given a
// different object than the one they asked for, and no way to know it.
BOOST_AUTO_TEST_CASE(AnEmptySegmentIsRefused) {
    BOOST_TEST(!ResolveUploadKey(route(), "/upload/a//b.xml").valid);
}

BOOST_AUTO_TEST_CASE(ANullByteIsRefused) {
    BOOST_TEST(!ResolveUploadKey(route(), "/upload/a%00b.xml").valid);
}

// ── Naming nothing ──────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(APutToTheRouteItselfNamesNoObject) {
    const auto resolved = ResolveUploadKey(route(), "/upload");

    BOOST_TEST(!resolved.valid);
    // The message says what to do instead, since this is the mistake somebody makes on their first
    // attempt and a bare "bad request" would not help them.
    BOOST_TEST(resolved.reason.find("/upload/") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ATrailingSlashNamesNoObjectEither) {
    BOOST_TEST(!ResolveUploadKey(route(), "/upload/").valid);
}

// A key may have as many segments as the caller likes - a bucket's keys are paths in name only,
// and nothing here is a directory.
BOOST_AUTO_TEST_CASE(ADeepKeyIsFine) {
    BOOST_TEST(ResolveUploadKey(route(), "/upload/a/b/c/d/e/f.xml").key == "a/b/c/d/e/f.xml");
}
