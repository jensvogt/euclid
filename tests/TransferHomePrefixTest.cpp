#define BOOST_TEST_MODULE TransferHomePrefixTest
#include <boost/test/unit_test.hpp>

// Euclid includes
#include <TransferPaths.h>

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
