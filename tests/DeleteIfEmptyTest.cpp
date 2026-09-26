// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE DeleteIfEmptyTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <string>

// Euclid includes
#include <euclid/database/Database.h>
#include <euclid/database/EventBus.h>
#include <euclid/database/repository/esm/MongoEsmRepository.h>
#include <euclid/dto/eqs/DeleteQueueRequest.h>
#include <euclid/dto/esm/DeleteBucketRequest.h>

using Euclid::Database::EventBus;
using Euclid::Database::MongoEsmRepository;
using Euclid::Database::Entity::ESM::Bucket;
using Euclid::Database::Entity::ESM::Object;

// delete-bucket takes the bucket's objects with it and delete-queue discards the queue's messages.
// That is right for an operator who has decided and wrong for a provisioning run, where removing a
// line from a manifest would otherwise be one call away from destroying a bucket of deliveries.
// --if-empty is what an automated caller asks for instead, and these are the three things its
// correctness rests on: what counts as empty, that asking does not itself destroy anything, and
// that the flag defaults to off so nothing that worked before behaves differently.

namespace {

    constexpr auto kBucketErn = "ern:esm:eu-central-1:000000000000:development:bucket:transfer-server";
    constexpr auto kQueueErn = "ern:eqs:eu-central-1:000000000000:development:queue:parsing-in";

    MongoEsmRepository freshRepository() {
        Euclid::Database::Database::instance().initializeMemory();
        return MongoEsmRepository{};
    }

    void storeBucket(MongoEsmRepository &repo) {
        Bucket bucket;
        bucket.name = "transfer-server";
        bucket.ern = kBucketErn;
        bucket.accountId = "000000000000";
        bucket.nameSpace = "development";
        bucket.region = "eu-central-1";
        bucket.owner = "admin";
        std::ignore = repo.upsertBucket(bucket);
    }

    void storeObject(MongoEsmRepository &repo, const std::string &key, const long size) {
        Object object;
        object.bucketErn = kBucketErn;
        object.key = key;
        object.size = size;
        object.accountId = "000000000000";
        object.nameSpace = "development";
        object.region = "eu-central-1";
        // The shape ESM gives an object: the bucket's name and the key, under the object type. The
        // store enforces uniqueness on it, so two objects without one are one object.
        object.ern = "ern:esm:eu-central-1:000000000000:development:object:transfer-server/" + key;
        std::ignore = repo.upsertObject(object);
    }

}// namespace

BOOST_AUTO_TEST_SUITE(DeleteIfEmptyTest)

BOOST_AUTO_TEST_CASE(a_directory_marker_makes_a_bucket_not_empty) {

    auto repo = freshRepository();
    storeBucket(repo);

    BOOST_TEST(repo.countObjects(kBucketErn, "", true) == 0);

    // A transfer server's directory is a zero-byte object whose key ends in "/". A listing hides
    // it, which is exactly why the count must not: a bucket holding somebody's folder structure is
    // in use, and deleting it silently takes the structure with it.
    storeObject(repo, "incoming/", 0);

    BOOST_TEST(repo.countObjects(kBucketErn, "", true) == 1);

    // Counting the way a listing does would call this bucket empty - the mistake this test exists
    // to prevent.
    BOOST_TEST(repo.countObjects(kBucketErn, "", false) == 0);
}

BOOST_AUTO_TEST_CASE(objects_make_a_bucket_not_empty) {

    auto repo = freshRepository();
    storeBucket(repo);
    storeObject(repo, "incoming/mix/PIM-4269.xml", 45242);
    storeObject(repo, "feedback/report.csv", 512);

    BOOST_TEST(repo.countObjects(kBucketErn, "", true) == 2);

    // And the count is per bucket - a full bucket next door does not protect an empty one, nor an
    // empty one condemn a full one.
    BOOST_TEST(repo.countObjects("ern:esm:eu-central-1:000000000000:development:bucket:other", "", true) == 0);
}

BOOST_AUTO_TEST_CASE(counting_deliveries_does_not_discard_them) {

    Euclid::Database::Database::instance().initializeMemory();

    // PendingDeliveries() exists because the only question of this shape that could be asked was
    // DiscardDeliveries(), which answers by deleting - no use at all to a caller deciding whether
    // deleting is allowed. So the first thing worth pinning is that asking is not doing.
    const auto before = EventBus::instance().PendingDeliveries(kQueueErn);
    BOOST_TEST(before >= 0);

    BOOST_TEST(EventBus::instance().PendingDeliveries(kQueueErn) == before);

    // An empty ERN is nothing waiting rather than everything waiting, and must not be read as
    // could-not-tell either.
    BOOST_TEST(EventBus::instance().PendingDeliveries("") == 0);
}

BOOST_AUTO_TEST_CASE(if_empty_is_off_unless_asked_for) {

    // The flag travels in the request body, and an older client does not send it. Defaulting to
    // false is what keeps "delete-bucket" meaning what it has always meant; defaulting the other
    // way would turn every existing caller's delete into a refusal.
    const auto bucket = Euclid::Dto::ESM::DeleteBucketRequest::fromJson(R"({"ern":")" + std::string(kBucketErn) + R"("})");
    BOOST_TEST(bucket.ern == kBucketErn);
    BOOST_TEST(!bucket.ifEmpty);

    const auto queue = Euclid::Dto::EQS::DeleteQueueRequest::fromJson(R"({"ern":")" + std::string(kQueueErn) + R"("})");
    BOOST_TEST(queue.ern == kQueueErn);
    BOOST_TEST(!queue.ifEmpty);
}

BOOST_AUTO_TEST_CASE(if_empty_survives_the_round_trip) {

    // Both directions, because the CLI serializes and the module parses, and a flag that only
    // survives one of those is a guard that silently is not there.
    Euclid::Dto::ESM::DeleteBucketRequest bucket;
    bucket.ern = kBucketErn;
    bucket.ifEmpty = true;
    BOOST_TEST(Euclid::Dto::ESM::DeleteBucketRequest::fromJson(bucket.toJson()).ifEmpty);

    Euclid::Dto::EQS::DeleteQueueRequest queue;
    queue.ern = kQueueErn;
    queue.ifEmpty = true;
    BOOST_TEST(Euclid::Dto::EQS::DeleteQueueRequest::fromJson(queue.toJson()).ifEmpty);
}

BOOST_AUTO_TEST_SUITE_END()
