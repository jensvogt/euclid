#define BOOST_TEST_MODULE EsmDirectoryListingTest
#include <boost/test/unit_test.hpp>

// Euclid includes
#include <euclid/database/entity/esm/Object.h>
#include <euclid/database/repository/esm/MemoryEsmRepository.h>

using Euclid::Database::Entity::ESM::IsDirectoryKey;
using Euclid::Database::Entity::ESM::Object;
using Euclid::Database::MemoryEsmRepository;

// A directory is a zero-byte object whose key ends in "/" - the marker an FTP/SFTP transfer
// server writes on MKD, since a flat bucket has nothing else that could carry an empty
// directory's name. Every other consumer (RUI, CLI, SDKs) wants a bucket to report the files it
// holds, so listings and counts leave markers out unless they are asked for.

namespace {

    constexpr auto kBucketErn = "ern:esm:eu-central-1:000000000000:development:bucket:transfer";

    void store(MemoryEsmRepository &repo, const std::string &key, const long size) {
        Object object;
        object.bucketErn = kBucketErn;
        object.key = key;
        object.size = size;
        repo.upsertObject(object);
    }

    // The repository holds a mutex, so it is neither copyable nor movable - each case fills its
    // own instance in place rather than taking one back from a factory.
    void populate(MemoryEsmRepository &repo) {
        store(repo, "mix/", 0);
        store(repo, "mix/PIM-4269.xml", 45242);
        store(repo, "mix/empty/", 0);
        store(repo, "report.json", 12);
    }

}// namespace

BOOST_AUTO_TEST_CASE(DirectoryKeyIsTheTrailingSlash) {
    BOOST_TEST(IsDirectoryKey("mix/"));
    BOOST_TEST(IsDirectoryKey("mix/empty/"));
    BOOST_TEST(!IsDirectoryKey("mix/PIM-4269.xml"));
    BOOST_TEST(!IsDirectoryKey("/leading-slash-is-not-a-directory"));
    BOOST_TEST(!IsDirectoryKey(""));
}

BOOST_AUTO_TEST_CASE(ListingLeavesDirectoriesOutByDefault) {
    MemoryEsmRepository repo;
    populate(repo);

    const auto files = repo.listObjects(kBucketErn, "", -1, -1, "key", "asc", false);
    BOOST_TEST_REQUIRE(files.size() == 2U);
    BOOST_TEST(files[0].key == "mix/PIM-4269.xml");
    BOOST_TEST(files[1].key == "report.json");

    BOOST_TEST(repo.countObjects(kBucketErn, "", false) == 2);
    BOOST_TEST(repo.countObjects(kBucketErn, "mix/", false) == 1);
}

BOOST_AUTO_TEST_CASE(ListingIncludesDirectoriesWhenAsked) {
    // What a transfer server sees: without the markers, "mix/empty" would not exist at all.
    MemoryEsmRepository repo;
    populate(repo);

    const auto all = repo.listObjects(kBucketErn, "", -1, -1, "key", "asc", true);
    BOOST_TEST_REQUIRE(all.size() == 4U);
    BOOST_TEST(all[0].key == "mix/");
    BOOST_TEST(all[1].key == "mix/PIM-4269.xml");
    BOOST_TEST(all[2].key == "mix/empty/");

    BOOST_TEST(repo.countObjects(kBucketErn, "", true) == 4);
    BOOST_TEST(repo.countObjects(kBucketErn, "mix/", true) == 3);
}

BOOST_AUTO_TEST_CASE(PagingAppliesAfterDirectoriesAreFilteredOut) {
    // The filter has to run before the page is cut, or a page of files could come back short -
    // or empty - because markers took up the slots.
    MemoryEsmRepository repo;
    populate(repo);

    const auto firstPage = repo.listObjects(kBucketErn, "", 1, 0, "key", "asc", false);
    BOOST_TEST_REQUIRE(firstPage.size() == 1U);
    BOOST_TEST(firstPage[0].key == "mix/PIM-4269.xml");

    const auto secondPage = repo.listObjects(kBucketErn, "", 1, 1, "key", "asc", false);
    BOOST_TEST_REQUIRE(secondPage.size() == 1U);
    BOOST_TEST(secondPage[0].key == "report.json");
}
