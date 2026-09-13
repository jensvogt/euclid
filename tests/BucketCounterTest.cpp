// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE BucketCounterTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <string>
#include <thread>
#include <vector>

// Euclid includes
#include <euclid/database/Database.h>
#include <euclid/database/repository/esm/MongoEsmRepository.h>

using Euclid::Database::MongoEsmRepository;
using Euclid::Database::Entity::ESM::Bucket;

// A bucket's object count and byte total used to be maintained by reading the bucket, subtracting,
// and writing the whole document back. Two of those overlapping lose one of the two adjustments -
// and overlapping is the normal case, not a rare one: purge-bucket, delete-objects and
// delete-bucket each run on a detached thread when asked with --async, so several can be in flight
// at once, and put-object moves the same two fields on every upload while they run.
//
// Worse than the arithmetic: upsertBucket() writes the *whole* document from the caller's copy, so
// a stale copy also reverted every other field - a tag added, encryption enabled - while it was
// adjusting a number.
//
// adjustBucketCounters() does the arithmetic in the database and touches nothing else.

namespace {

    constexpr auto kErn = "ern:esm:eu-central-1:000000000000:production:bucket:dropbox";

    MongoEsmRepository freshRepository() {
        Euclid::Database::Database::instance().initializeMemory();
        return MongoEsmRepository{};
    }

    Bucket bucketOf(MongoEsmRepository &repo, const long size = 0, const long objects = 0) {
        Bucket bucket;
        bucket.name = "dropbox";
        bucket.ern = kErn;
        bucket.accountId = "000000000000";
        bucket.nameSpace = "production";
        bucket.region = "eu-central-1";
        bucket.owner = "jvo";
        bucket.size = size;
        bucket.objects = objects;
        return repo.upsertBucket(bucket);
    }

    Bucket reread(const MongoEsmRepository &repo) {
        const auto found = repo.findBucketByErn(kErn);
        BOOST_REQUIRE(found.has_value());
        return *found;
    }

}// namespace

BOOST_AUTO_TEST_CASE(AnAdjustmentMovesBothCounters) {

    auto repo = freshRepository();
    std::ignore = bucketOf(repo, 1000, 4);

    repo.adjustBucketCounters(kErn, 250, 1);

    const auto after = reread(repo);
    BOOST_TEST(after.size == 1250L);
    BOOST_TEST(after.objects == 5L);
}

BOOST_AUTO_TEST_CASE(ASubtractionMovesThemBack) {

    auto repo = freshRepository();
    std::ignore = bucketOf(repo, 1000, 4);

    repo.adjustBucketCounters(kErn, -400, -2);

    const auto after = reread(repo);
    BOOST_TEST(after.size == 600L);
    BOOST_TEST(after.objects == 2L);
}

BOOST_AUTO_TEST_CASE(NeitherCounterGoesBelowZero) {

    // Only reachable if something was counted twice, which is a defect rather than a race now the
    // arithmetic is atomic - so it is clamped where it is found and the next adjustment starts
    // from a sane figure instead of compounding.
    auto repo = freshRepository();
    std::ignore = bucketOf(repo, 100, 1);

    repo.adjustBucketCounters(kErn, -500, -9);

    const auto after = reread(repo);
    BOOST_TEST(after.size == 0L);
    BOOST_TEST(after.objects == 0L);
}

BOOST_AUTO_TEST_CASE(AdjustingByNothingDoesNothing) {

    auto repo = freshRepository();
    std::ignore = bucketOf(repo, 1000, 4);

    repo.adjustBucketCounters(kErn, 0, 0);

    const auto after = reread(repo);
    BOOST_TEST(after.size == 1000L);
    BOOST_TEST(after.objects == 4L);
}

BOOST_AUTO_TEST_CASE(AnAdjustmentLeavesEveryOtherFieldAlone) {

    // The half of this that is not about arithmetic. A purge adjusting a counter must not revert
    // a tag somebody added, or the encryption key somebody enabled, while it does so.
    auto repo = freshRepository();
    auto bucket = bucketOf(repo, 1000, 4);
    bucket.encryptionKeyErn = "ern:ekm:eu-central-1:000000000000:production:key:archive";
    bucket.internal = true;
    std::ignore = repo.upsertBucket(bucket);

    repo.adjustBucketCounters(kErn, -1000, -4);

    const auto after = reread(repo);
    BOOST_TEST(after.encryptionKeyErn == "ern:ekm:eu-central-1:000000000000:production:key:archive");
    BOOST_TEST(after.internal);
    BOOST_TEST(after.owner == "jvo");
    BOOST_TEST(after.name == "dropbox");
}

// The case the whole change exists for.
BOOST_AUTO_TEST_CASE(ConcurrentAdjustmentsAllLand) {

    // Sixteen threads each adding a hundred objects and then removing them again: every
    // adjustment has to be counted, and the total has to come back to where it started. Under
    // read-subtract-write this loses adjustments and settles on an arbitrary number.
    constexpr int kThreads = 16;
    constexpr int kPerThread = 100;

    auto repo = freshRepository();
    std::ignore = bucketOf(repo, 0, 0);

    std::vector<std::thread> adders;
    adders.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        adders.emplace_back([&repo] {
            for (int i = 0; i < kPerThread; ++i) repo.adjustBucketCounters(kErn, 10, 1);
        });
    }
    for (auto &thread: adders) thread.join();

    const auto afterAdding = reread(repo);
    BOOST_TEST(afterAdding.objects == static_cast<long>(kThreads * kPerThread));
    BOOST_TEST(afterAdding.size == static_cast<long>(kThreads * kPerThread * 10));

    // And back down, which is the purge direction - several async purges on one bucket at once.
    std::vector<std::thread> removers;
    removers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        removers.emplace_back([&repo] {
            for (int i = 0; i < kPerThread; ++i) repo.adjustBucketCounters(kErn, -10, -1);
        });
    }
    for (auto &thread: removers) thread.join();

    const auto afterRemoving = reread(repo);
    BOOST_TEST(afterRemoving.objects == 0L);
    BOOST_TEST(afterRemoving.size == 0L);
}
