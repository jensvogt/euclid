#define BOOST_TEST_MODULE TransferHomePrefixTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <string>
#include <vector>

// Euclid includes
#include <TransferPaths.h>

using Euclid::Transfer::HomeDirectoryKeys;
using Euclid::Transfer::HomePrefix;

// A transfer server fronts one bucket, so without a home directory every client of it shares one
// flat key space: the key of an upload is exactly the path the client typed, two users can
// overwrite each other, and nothing in the key says who delivered the file. HomePrefix is what
// roots each session somewhere of its own, and the normalisation below is what lets its result be
// concatenated with a key without any caller thinking about slashes.

BOOST_AUTO_TEST_CASE(EmptyTemplateIsTheBucketRoot) {
    BOOST_CHECK_EQUAL(HomePrefix("", "ftpuser1"), "");
}

BOOST_AUTO_TEST_CASE(UserPlaceholderIsSubstituted) {
    BOOST_CHECK_EQUAL(HomePrefix("{user}", "ftpuser1"), "ftpuser1/");
}

BOOST_AUTO_TEST_CASE(NestsUnderACommonPrefix) {
    BOOST_CHECK_EQUAL(HomePrefix("lieferanten/{user}", "DLI145"), "lieferanten/DLI145/");
}

BOOST_AUTO_TEST_CASE(SubstitutesEveryOccurrence) {
    BOOST_CHECK_EQUAL(HomePrefix("{user}/incoming/{user}", "dli"), "dli/incoming/dli/");
}

// The same prefix however it was written down, so an admin does not have to guess which spelling
// the server expects.
BOOST_AUTO_TEST_CASE(SlashesAreNormalised) {
    BOOST_CHECK_EQUAL(HomePrefix("/{user}", "ftpuser1"), "ftpuser1/");
    BOOST_CHECK_EQUAL(HomePrefix("{user}/", "ftpuser1"), "ftpuser1/");
    BOOST_CHECK_EQUAL(HomePrefix("//{user}//", "ftpuser1"), "ftpuser1/");
    BOOST_CHECK_EQUAL(HomePrefix("/", "ftpuser1"), "");
}

// A literal template needs no user at all - a server can put every client in one shared folder.
BOOST_AUTO_TEST_CASE(TemplateWithoutPlaceholderIsUsedLiterally) {
    BOOST_CHECK_EQUAL(HomePrefix("incoming", "ftpuser1"), "incoming/");
}

// ".." is dropped rather than honoured: a home directory can only ever name a prefix below the
// bucket root, so no template can be written that reaches a sibling's files.
BOOST_AUTO_TEST_CASE(DotSegmentsCannotEscape) {
    BOOST_CHECK_EQUAL(HomePrefix("{user}/..", "ftpuser1"), "ftpuser1/");
    BOOST_CHECK_EQUAL(HomePrefix("../../etc", "ftpuser1"), "etc/");
    BOOST_CHECK_EQUAL(HomePrefix("./{user}", "ftpuser1"), "ftpuser1/");
    BOOST_CHECK_EQUAL(HomePrefix("..", "ftpuser1"), "");
}

// An empty user id would otherwise leave the placeholder standing in the key, which is worse
// than the bucket root: every such session would share a folder literally named "{user}".
BOOST_AUTO_TEST_CASE(EmptyUserLeavesNoPlaceholderBehind) {
    BOOST_CHECK_EQUAL(HomePrefix("{user}", ""), "");
    BOOST_CHECK_EQUAL(HomePrefix("lieferanten/{user}", ""), "lieferanten/");
}

// ── HomeDirectoryKeys ───────────────────────────────────────────────────────
// The folder skeleton a session finds under its home. Every intermediate level is a key of its
// own, because a directory marker is one object: without "incoming/" standing for itself, a
// client has nothing to walk into on its way to "incoming/mix".

BOOST_AUTO_TEST_CASE(NestedDirectoriesIncludeTheirParents) {
    const std::vector<std::string> expected{"jvo/incoming/", "jvo/incoming/mix/"};
    BOOST_TEST(HomeDirectoryKeys("jvo/", {"incoming/mix"}) == expected);
}

// Two siblings name their shared parent once between them, rather than each asking for it.
BOOST_AUTO_TEST_CASE(SharedParentsAreNotRepeated) {
    const std::vector<std::string> expected{"jvo/incoming/", "jvo/incoming/mix/", "jvo/incoming/split/", "jvo/feedback/"};
    BOOST_TEST(HomeDirectoryKeys("jvo/", {"incoming/mix", "incoming/split", "feedback"}) == expected);
}

// Parents before children: they are created in this order, and a marker cannot be walked into
// before the one above it exists.
BOOST_AUTO_TEST_CASE(ParentsComeBeforeChildren) {
    const auto keys = HomeDirectoryKeys("", {"a/b/c"});
    const std::vector<std::string> expected{"a/", "a/b/", "a/b/c/"};
    BOOST_TEST(keys == expected);
}

// An empty home is the bucket root, which is what every installation had before a home directory
// was configured - the skeleton is still created, just not under anything.
BOOST_AUTO_TEST_CASE(EmptyHomeIsTheBucketRoot) {
    const std::vector<std::string> expected{"feedback/"};
    BOOST_TEST(HomeDirectoryKeys("", {"feedback"}) == expected);
}

// Nothing configured creates nothing, which is the default for every installation that has not
// asked for a skeleton.
BOOST_AUTO_TEST_CASE(NothingConfiguredCreatesNothing) {
    BOOST_TEST(HomeDirectoryKeys("jvo/", {}).empty());
}

// Normalised exactly as a home template is, so an admin does not have to guess the spelling.
BOOST_AUTO_TEST_CASE(SlashesAreNormalisedLikeHomePrefix) {
    const std::vector<std::string> expected{"jvo/incoming/"};
    BOOST_TEST(HomeDirectoryKeys("jvo/", {"/incoming/"}) == expected);
    BOOST_TEST(HomeDirectoryKeys("jvo/", {"//incoming//"}) == expected);
}

// A configured directory cannot climb out of the home it belongs to - the same guarantee
// HomePrefix gives its template, and for the same reason.
BOOST_AUTO_TEST_CASE(ConfiguredDirectoriesCannotEscapeTheHome) {
    const std::vector<std::string> expected{"jvo/etc/"};
    BOOST_TEST(HomeDirectoryKeys("jvo/", {"../../etc"}) == expected);
    BOOST_TEST(HomeDirectoryKeys("jvo/", {"."}).empty());
    BOOST_TEST(HomeDirectoryKeys("jvo/", {".."}).empty());
}
