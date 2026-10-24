// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE EsmDirectoryListingTest
#include <boost/test/unit_test.hpp>

// Euclid includes
#include <euclid/core/ErnUtils.h>
#include <euclid/database/entity/esm/Object.h>
#include <euclid/database/Database.h>
#include <euclid/database/repository/esm/MongoEsmRepository.h>

using Euclid::Database::MongoEsmRepository;
using Euclid::Database::Entity::ESM::IsDirectoryKey;
using Euclid::Database::Entity::ESM::Object;

// A directory is a zero-byte object whose key ends in "/" - the marker an FTP/SFTP transfer
// server writes on MKD, since a flat bucket has nothing else that could carry an empty
// directory's name. Every other consumer (RUI, CLI, SDKs) wants a bucket to report the files it
// holds, so listings and counts leave markers out unless they are asked for.

namespace {

    constexpr auto kBucketErn = "ern:esm:eu-central-1:000000000000:development:bucket:transfer";

    void store(MongoEsmRepository &repo, const std::string &key, const long size) {
        Object object;
        object.bucketErn = kBucketErn;
        // An object carries its own ern, and the repository holds a unique index over it: the
        // module names every object it writes, so a test that left the field empty would be
        // storing two objects under one name and colliding on the second.
        object.ern = Euclid::Core::createEsmObjectErn("000000000000", "development", "transfer/" + key);
        object.key = key;
        object.size = size;
        repo.upsertObject(object);
    }

    // The repository holds a mutex, so it is neither copyable nor movable - each case fills its
    // own instance in place rather than taking one back from a factory.
    void populate(MongoEsmRepository &repo) {
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
    Euclid::Database::Database::instance().initializeMemory();
    MongoEsmRepository repo;
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
    Euclid::Database::Database::instance().initializeMemory();
    MongoEsmRepository repo;
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
    Euclid::Database::Database::instance().initializeMemory();
    MongoEsmRepository repo;
    populate(repo);

    const auto firstPage = repo.listObjects(kBucketErn, "", 1, 0, "key", "asc", false);
    BOOST_TEST_REQUIRE(firstPage.size() == 1U);
    BOOST_TEST(firstPage[0].key == "mix/PIM-4269.xml");

    const auto secondPage = repo.listObjects(kBucketErn, "", 1, 1, "key", "asc", false);
    BOOST_TEST_REQUIRE(secondPage.size() == 1U);
    BOOST_TEST(secondPage[0].key == "report.json");
}

namespace {

    // A tree whose folders are found only through what is inside them - "deep/" has no marker and
    // no object of its own, just a grandchild - next to siblings whose names sort around the
    // "/" separator ("a!", "a0"), which is where a skip scan that jumps past one folder's block
    // would swallow its neighbour if the bound were wrong.
    void populateTree(MongoEsmRepository &repo) {
        store(repo, "top.txt", 1);
        store(repo, "a/one.txt", 1);
        store(repo, "a/two.txt", 1);
        store(repo, "a/sub/", 0);
        store(repo, "a/sub/three.txt", 1);
        store(repo, "a!/bang.txt", 1);
        store(repo, "a0/zero.txt", 1);
        store(repo, "deep/x/y/z.txt", 1);
    }

}// namespace

BOOST_AUTO_TEST_CASE(SubdirectoriesAreTheFirstSegmentBelowTheLevel) {
    Euclid::Database::Database::instance().initializeMemory();
    MongoEsmRepository repo;
    populateTree(repo);

    const auto root = repo.listSubdirectories(kBucketErn, "");
    BOOST_TEST_REQUIRE(root.size() == 4U);
    BOOST_TEST(root[0] == "a!/");
    BOOST_TEST(root[1] == "a/");
    BOOST_TEST(root[2] == "a0/");
    BOOST_TEST(root[3] == "deep/");

    const auto a = repo.listSubdirectories(kBucketErn, "a/");
    BOOST_TEST_REQUIRE(a.size() == 1U);
    BOOST_TEST(a[0] == "a/sub/");

    // A folder that holds only folders is still a folder - found through its grandchild.
    const auto deep = repo.listSubdirectories(kBucketErn, "deep/");
    BOOST_TEST_REQUIRE(deep.size() == 1U);
    BOOST_TEST(deep[0] == "deep/x/");

    BOOST_TEST(repo.listSubdirectories(kBucketErn, "a/sub/").empty());
}

BOOST_AUTO_TEST_CASE(ALevelCountsAndListsWithoutItsMarkersUnlessAsked) {
    Euclid::Database::Database::instance().initializeMemory();
    MongoEsmRepository repo;
    populateTree(repo);

    BOOST_TEST(repo.countLevel(kBucketErn, "a/", false) == 2);
    BOOST_TEST(repo.countLevel(kBucketErn, "a/", true) == 3);

    const auto files = repo.listLevel(kBucketErn, "a/", -1, -1, "asc", false);
    BOOST_TEST_REQUIRE(files.size() == 2U);
    BOOST_TEST(files[0].key == "a/one.txt");
    BOOST_TEST(files[1].key == "a/two.txt");

    // Markers and files in one key order, not markers first: the listing merges the two.
    const auto all = repo.listLevel(kBucketErn, "a/", -1, -1, "asc", true);
    BOOST_TEST_REQUIRE(all.size() == 3U);
    BOOST_TEST(all[0].key == "a/one.txt");
    BOOST_TEST(all[1].key == "a/sub/");
    BOOST_TEST(all[2].key == "a/two.txt");

    const auto page = repo.listLevel(kBucketErn, "a/", 1, 1, "asc", false);
    BOOST_TEST_REQUIRE(page.size() == 1U);
    BOOST_TEST(page[0].key == "a/two.txt");
}
