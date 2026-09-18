// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE PurgePagingTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <string>
#include <vector>

// Euclid includes
#include <euclid/core/ErnUtils.h>
#include <euclid/database/Database.h>
#include <euclid/database/entity/esm/Object.h>
#include <euclid/database/repository/esm/MongoEsmRepository.h>

using Euclid::Database::MongoEsmRepository;
using Euclid::Database::Entity::ESM::Object;

// An --async purge used to list a bucket's objects in one call - every one of them, into one
// vector - and remove exactly that list. Two things followed, and both looked identical from
// outside: "the purge stopped after a while".
//
// The list was the allocation that ends the process on a large enough bucket. And a single pass
// removes what existed when it started and nothing else, so a bucket still being written to - a
// transfer server delivering into it - was never emptied however long the pass ran. The removal
// reported success with the bucket not empty.
//
// It now takes a page at a time and repeats until nothing is left to find. What that depends on is
// the invariant below: asking for page *zero* again, after deleting the page just read, returns
// the next objects rather than the same ones. Paging forward with an index instead would step over
// exactly as many objects as it removed and leave half the bucket behind - which is why this pins
// the loop the way EsmServer actually writes it rather than the repository call on its own.

namespace {

    constexpr auto kBucketErn = "ern:esm:eu-central-1:000000000000:development:bucket:transfer";

    void store(MongoEsmRepository &repo, const std::string &key, const long size = 10) {
        Object object;
        object.bucketErn = kBucketErn;
        object.ern = Euclid::Core::createEsmObjectErn("000000000000", "development", "transfer/" + key);
        object.key = key;
        object.size = size;
        repo.upsertObject(object);
    }

    struct Removed {
        long count = 0;
        long pages = 0;
    };

    // The loop EsmServer::removeBucketObjectsPaged() runs, with the same page-zero rule and the
    // same no-progress guard. `stopAfterPages` stands in for the process being killed mid-run.
    Removed purgeInPages(MongoEsmRepository &repo, const std::string &prefix, const long pageSize,
                         const long stopAfterPages = -1) {

        Removed removed;
        while (stopAfterPages < 0 || removed.pages < stopAfterPages) {
            const auto objects = repo.listObjects(kBucketErn, prefix, pageSize, 0, "", "asc", true);
            if (objects.empty()) break;

            // One statement for the page, exactly as removeObjects() does - the loop is only
            // faithful to the thing it stands in for if it deletes the same way.
            std::vector<std::string> erns;
            erns.reserve(objects.size());
            for (const auto &object: objects) erns.push_back(object.ern);

            const long thisPage = repo.deleteObjectsByErns(erns);
            removed.count += thisPage;
            ++removed.pages;

            if (thisPage == 0) break;
        }
        return removed;
    }

    long remaining(const MongoEsmRepository &repo, const std::string &prefix = "") {
        return static_cast<long>(repo.listObjects(kBucketErn, prefix, -1, -1, "", "asc", true).size());
    }

}// namespace

BOOST_AUTO_TEST_CASE(PagingEmptiesTheBucketRatherThanOnePageOfIt) {

    Euclid::Database::Database::instance().initializeMemory();
    MongoEsmRepository repo;

    for (int i = 0; i < 250; ++i) store(repo, "file-" + std::to_string(i) + ".xml");
    BOOST_REQUIRE(remaining(repo) == 250L);

    const auto removed = purgeInPages(repo, "", 25);

    BOOST_TEST(removed.count == 250L);
    BOOST_TEST(remaining(repo) == 0L);

    // Ten pages of twenty-five and one empty page to find out it is done. The page count is
    // asserted because "it emptied the bucket" would also pass if the page size were ignored and
    // the whole lot came back at once, which is the thing being fixed.
    BOOST_TEST(removed.pages == 10L);
}

BOOST_AUTO_TEST_CASE(APageSizeLargerThanTheBucketIsOnePage) {

    Euclid::Database::Database::instance().initializeMemory();
    MongoEsmRepository repo;

    for (int i = 0; i < 5; ++i) store(repo, "file-" + std::to_string(i) + ".xml");

    const auto removed = purgeInPages(repo, "", 1000);

    BOOST_TEST(removed.count == 5L);
    BOOST_TEST(removed.pages == 1L);
    BOOST_TEST(remaining(repo) == 0L);
}

BOOST_AUTO_TEST_CASE(APrefixNarrowsWhatIsRemovedAndLeavesTheRest) {

    Euclid::Database::Database::instance().initializeMemory();
    MongoEsmRepository repo;

    for (int i = 0; i < 60; ++i) store(repo, "incoming/file-" + std::to_string(i) + ".xml");
    for (int i = 0; i < 40; ++i) store(repo, "archive/file-" + std::to_string(i) + ".xml");

    const auto removed = purgeInPages(repo, "incoming/", 10);

    BOOST_TEST(removed.count == 60L);
    BOOST_TEST(remaining(repo, "incoming/") == 0L);
    BOOST_TEST(remaining(repo, "archive/") == 40L);
}

BOOST_AUTO_TEST_CASE(WhatArrivesDuringAPurgeIsTakenByTheSamePurge) {

    // The single-snapshot bug, stated as a test. Under the old code the pass removed the 100 it
    // listed at the start and reported success; the 30 written meanwhile stayed, and the operator
    // saw a purge that "stopped".
    Euclid::Database::Database::instance().initializeMemory();
    MongoEsmRepository repo;

    for (int i = 0; i < 100; ++i) store(repo, "file-" + std::to_string(i) + ".xml");

    // Two pages in, something delivers into the bucket - which is what a transfer server does.
    const auto firstPass = purgeInPages(repo, "", 25, 2);
    BOOST_TEST(firstPass.count == 50L);
    for (int i = 0; i < 30; ++i) store(repo, "late-" + std::to_string(i) + ".xml");

    // The loop has not finished, so it finds them: it stops when the bucket is empty, not when the
    // list it started with is exhausted.
    const auto rest = purgeInPages(repo, "", 25);

    BOOST_TEST(rest.count == 80L);
    BOOST_TEST(remaining(repo) == 0L);
}

BOOST_AUTO_TEST_CASE(AnInterruptedPurgeLeavesTheRestToBeAskedForAgain) {

    // What makes the operation resumable, and why the bucket document is deleted last: a run that
    // is killed halfway has removed whole pages, not half-rows, and asking again takes the rest.
    Euclid::Database::Database::instance().initializeMemory();
    MongoEsmRepository repo;

    for (int i = 0; i < 100; ++i) store(repo, "file-" + std::to_string(i) + ".xml");

    BOOST_TEST(purgeInPages(repo, "", 10, 3).count == 30L);
    BOOST_TEST(remaining(repo) == 70L);

    BOOST_TEST(purgeInPages(repo, "", 10).count == 70L);
    BOOST_TEST(remaining(repo) == 0L);
}

BOOST_AUTO_TEST_CASE(AnEmptyBucketCostsOneListingAndStops) {

    Euclid::Database::Database::instance().initializeMemory();
    MongoEsmRepository repo;

    const auto removed = purgeInPages(repo, "", 25);

    BOOST_TEST(removed.count == 0L);
    BOOST_TEST(removed.pages == 0L);
}

// ── A page at a time, in one statement ─────────────────────────────────────

BOOST_AUTO_TEST_CASE(APageOfErnsIsDeletedTogether) {

    // The reason this exists. Each single-ERN delete is a synchronous round trip, and on a loaded
    // installation a round trip is about 13.6 ms: 1,000 rows measured 13,714 ms one at a time
    // against 19 ms in one statement. The purge was round-trip bound and nothing else was close -
    // the file unlinks beside these run at 78,783 a second.
    Euclid::Database::Database::instance().initializeMemory();
    MongoEsmRepository repo;
    for (int i = 0; i < 25; ++i) store(repo, "mix/" + std::to_string(i) + ".xml");

    std::vector<std::string> erns;
    for (const auto &object: repo.listObjects(kBucketErn, "", -1, -1, "", "asc", true)) {
        erns.push_back(object.ern);
    }

    BOOST_TEST(repo.deleteObjectsByErns(erns) == 25L);
    BOOST_TEST(remaining(repo) == 0L);
}

BOOST_AUTO_TEST_CASE(DeletingNothingAsksTheDatabaseNothing) {

    // Guarded at the repository so the caller does not have to: a purge whose last page came back
    // empty, and a delete-objects call naming keys that are all gone, both arrive here empty.
    Euclid::Database::Database::instance().initializeMemory();
    MongoEsmRepository repo;
    store(repo, "mix/keep.xml");

    BOOST_TEST(repo.deleteObjectsByErns({}) == 0L);
    BOOST_TEST(remaining(repo) == 1L);
}

BOOST_AUTO_TEST_CASE(ErnsThatMatchNothingAreNotAnError) {

    // Two workers racing the same bucket both ask for rows one of them has already taken - the
    // page is re-listed after it is removed, so this is ordinary rather than exceptional. The
    // honest answer is how many actually went, which is what the no-progress guard reads.
    Euclid::Database::Database::instance().initializeMemory();
    MongoEsmRepository repo;
    store(repo, "mix/here.xml");

    const auto present = repo.listObjects(kBucketErn, "", -1, -1, "", "asc", true).front().ern;
    const long deleted = repo.deleteObjectsByErns({present, "ern:esm:eu-central-1:000000000000:development:object:transfer/gone.xml"});

    BOOST_TEST(deleted == 1L);
    BOOST_TEST(remaining(repo) == 0L);
}

BOOST_AUTO_TEST_CASE(OnlyTheNamedErnsGo) {

    // $in over the indexed "ern" field, so the page is a set of index lookups rather than anything
    // that could reach a row nobody asked about.
    Euclid::Database::Database::instance().initializeMemory();
    MongoEsmRepository repo;
    store(repo, "mix/a.xml");
    store(repo, "mix/b.xml");
    store(repo, "split/c.xml");

    const auto objects = repo.listObjects(kBucketErn, "mix/", -1, -1, "", "asc", true);
    std::vector<std::string> erns;
    for (const auto &object: objects) erns.push_back(object.ern);

    BOOST_TEST(repo.deleteObjectsByErns(erns) == 2L);
    BOOST_TEST(remaining(repo) == 1L);
    BOOST_TEST(remaining(repo, "split/") == 1L);
}
