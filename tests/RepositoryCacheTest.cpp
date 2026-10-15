// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE RepositoryCacheTest
#include <boost/test/unit_test.hpp>

#include <string>

#include <euclid/database/Database.h>
#include <euclid/database/repository/ens/MongoEnsRepository.h>
#include <euclid/database/repository/eqs/MongoEqsRepository.h>
#include <euclid/database/repository/esm/MongoEsmRepository.h>

using Euclid::Database::MongoEnsRepository;
using Euclid::Database::MongoEqsRepository;
using Euclid::Database::MongoEsmRepository;
using Euclid::Database::Entity::EQS::Queue;
using Euclid::Database::Entity::EQS::QueueStatus;

// The caches in front of the per-message lookups - a queue by its ERN on every send, receive and
// delivery, and a bucket's or topic's subscriptions on every object event and publish.
//
// What is pinned is that a change made through this process is seen at once, through the cached
// lookup, without waiting out the TTL: a queue stopped, created or deleted, a subscription added or
// removed. Another process's changes are seen when the entry expires, which is the trade the caches
// make and not something a single-process test can show.

namespace {

    constexpr auto kQueue = "ern:eqs:eu-central-1:000000000000:production:queue:orders";
    constexpr auto kBucket = "ern:esm:eu-central-1:000000000000:production:bucket:invoices";
    constexpr auto kTopic = "ern:ens:eu-central-1:000000000000:production:topic:orders";

    // The caches belong to the process, not to the database, so resetting the database between
    // cases is not enough on its own: clearQueues() also forgets every queue this process cached.
    // Subscriptions are reset by each case removing what it made.
    void freshDatabase() {
        Euclid::Database::Database::instance().initializeMemory();
        MongoEqsRepository{}.clearQueues();
    }

    Queue queue(MongoEqsRepository &repo, const QueueStatus status = QueueStatus::AVAILABLE) {
        Queue queue;
        queue.name = "orders";
        queue.ern = kQueue;
        queue.accountId = "000000000000";
        queue.nameSpace = "production";
        queue.region = "eu-central-1";
        queue.visibility = 30;
        queue.status = status;
        return repo.upsertQueue(queue);
    }

    template<typename Subscription, typename Repository>
    Subscription subscribe(Repository &repo, const std::string &source, const std::string &target) {
        Subscription subscription;
        subscription.ern = source + "/" + target;
        subscription.sourceErn = source;
        subscription.type = "SQS";
        subscription.targetErn = target;
        return repo.upsertSubscription(subscription);
    }

}// namespace

// ── Queues ──────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(AStoppedQueueIsSeenStoppedAtOnce) {

    // The receive's stopped check reads the cache on every poll; a stop made here must not wait for
    // the entry to run out before consumers are told.
    freshDatabase();
    MongoEqsRepository repo;
    queue(repo);
    BOOST_TEST_REQUIRE(repo.findQueueByErnCached(kQueue).has_value());

    queue(repo, QueueStatus::STOPPED);

    BOOST_TEST((repo.findQueueByErnCached(kQueue)->status == QueueStatus::STOPPED));
}

BOOST_AUTO_TEST_CASE(AQueueCreatedAfterAMissIsFoundAtOnce) {

    // "No such queue" is never remembered: a delivery that hears it discards the queue's pending
    // events, so a queue created a moment after a miss has to be found by the very next lookup.
    freshDatabase();
    MongoEqsRepository repo;
    BOOST_TEST(!repo.findQueueByErnCached(kQueue).has_value());

    queue(repo);

    BOOST_TEST(repo.findQueueByErnCached(kQueue).has_value());
}

BOOST_AUTO_TEST_CASE(ADeletedQueueIsGoneAtOnce) {
    freshDatabase();
    MongoEqsRepository repo;
    queue(repo);
    BOOST_TEST_REQUIRE(repo.findQueueByErnCached(kQueue).has_value());

    repo.deleteQueueByErn(kQueue);

    BOOST_TEST(!repo.findQueueByErnCached(kQueue).has_value());
}

// ── Bucket subscriptions ────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(ABucketSubscriptionIsSeenAtOnceEvenAfterNoneWasCached) {

    // "No subscriptions" is the answer for most buckets and is cached like any other - which makes
    // this the case that matters: a listener subscribing to a bucket whose emptiness is already
    // remembered must be told about the very next object.
    freshDatabase();
    MongoEsmRepository repo;
    BOOST_TEST(repo.listSubscriptionsBySourceErnCached(kBucket).empty());

    subscribe<Euclid::Database::Entity::ESM::Subscription>(repo, kBucket, kQueue);

    BOOST_TEST(repo.listSubscriptionsBySourceErnCached(kBucket).size() == 1u);
}

BOOST_AUTO_TEST_CASE(ARemovedBucketSubscriptionIsGoneAtOnce) {
    freshDatabase();
    MongoEsmRepository repo;
    const auto subscription = subscribe<Euclid::Database::Entity::ESM::Subscription>(repo, kBucket, kQueue);
    BOOST_TEST_REQUIRE(repo.listSubscriptionsBySourceErnCached(kBucket).size() == 1u);

    repo.deleteSubscriptionByErn(subscription.ern);

    BOOST_TEST(repo.listSubscriptionsBySourceErnCached(kBucket).empty());
}

BOOST_AUTO_TEST_CASE(ARepointedSubscriptionMovesWithItsBucketAtOnce) {

    // A bucket rename repoints its subscriptions to the new ERN; both entries are stale after it.
    freshDatabase();
    MongoEsmRepository repo;
    const std::string renamed = std::string(kBucket) + "-renamed";
    subscribe<Euclid::Database::Entity::ESM::Subscription>(repo, kBucket, kQueue);
    BOOST_TEST_REQUIRE(repo.listSubscriptionsBySourceErnCached(kBucket).size() == 1u);
    BOOST_TEST_REQUIRE(repo.listSubscriptionsBySourceErnCached(renamed).empty());

    repo.repointSubscriptions(kBucket, renamed);

    BOOST_TEST(repo.listSubscriptionsBySourceErnCached(kBucket).empty());
    BOOST_TEST(repo.listSubscriptionsBySourceErnCached(renamed).size() == 1u);
}

// ── Topic subscriptions ─────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(ATopicSubscriptionIsSeenAtOnceAndGoneAtOnce) {
    freshDatabase();
    MongoEnsRepository repo;
    BOOST_TEST(repo.listSubscriptionsBySourceErnCached(kTopic).empty());

    const auto subscription = subscribe<Euclid::Database::Entity::ENS::Subscription>(repo, kTopic, kQueue);
    BOOST_TEST(repo.listSubscriptionsBySourceErnCached(kTopic).size() == 1u);

    repo.deleteSubscriptionByErn(subscription.ern);
    BOOST_TEST(repo.listSubscriptionsBySourceErnCached(kTopic).empty());
}
