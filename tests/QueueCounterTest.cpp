// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE QueueCounterTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <string>
#include <thread>
#include <vector>

// Euclid includes
#include <euclid/database/Database.h>
#include <euclid/database/repository/eqs/MongoEqsRepository.h>

using Euclid::Database::MongoEqsRepository;
using Euclid::Database::Entity::EQS::MessagePriority;
using Euclid::Database::Entity::EQS::Queue;

// What a queue says it holds used to be measured rather than maintained: every send, receive and
// delete left the counters alone, and a scan over every message in the installation put them right
// on a timer. That scan is not free and gets less free as the queues fill - on a queue half a
// million messages deep it was the most expensive thing in the database, running every minute.
//
// So the counters are maintained again, by the operations that change them. The two reasons they
// were taken out are answered rather than ignored: the writes are coalesced, so a queue's one row
// is not what every producer waits on; and the scan stays as a safety net, so a process that dies
// holding an adjustment no longer leaves the numbers permanently wrong.
//
// These cases are about the arithmetic being right for each kind of movement. The one thing they
// cannot see is the coalescing interval - flushQueueCounters() is what makes a test read its own
// writes, and is called for that reason wherever it appears below.

namespace {

    constexpr auto kQueue = "ern:eqs:eu-central-1:000000000000:production:queue:orders";
    constexpr auto kDeadLetterQueue = "ern:eqs:eu-central-1:000000000000:production:queue:orders-dlqueue";

    MongoEqsRepository freshRepository() {
        Euclid::Database::Database::instance().initializeMemory();
        return MongoEqsRepository{};
    }

    Queue queueOf(MongoEqsRepository &repo, const std::string &ern, const std::string &name) {
        Queue queue;
        queue.name = name;
        queue.ern = ern;
        queue.accountId = "000000000000";
        queue.nameSpace = "production";
        queue.region = "eu-central-1";
        queue.visibility = 30;
        queue.maxReceiveCount = 3;
        return repo.upsertQueue(queue);
    }

    Queue reread(const MongoEqsRepository &repo, const std::string &ern) {
        const auto stored = repo.findQueueByErn(ern);
        BOOST_TEST_REQUIRE(stored.has_value());
        return *stored;
    }

}// namespace

BOOST_AUTO_TEST_CASE(ASendCountsTheMessageAndItsBytes) {
    auto repo = freshRepository();
    queueOf(repo, kQueue, "orders");

    repo.sendMessage("m-1", "ern:eqs:...:message:m-1", kQueue, "0123456789", {}, {}, MessagePriority::MEDIUM);
    repo.flushQueueCounters();

    const auto queue = reread(repo, kQueue);
    BOOST_TEST(queue.available == 1);
    BOOST_TEST(queue.size == 10);
    BOOST_TEST(queue.invisible == 0);
    BOOST_TEST(queue.delayed == 0);
}

BOOST_AUTO_TEST_CASE(AReceiveMovesMessagesFromAvailableToInvisible) {
    auto repo = freshRepository();
    queueOf(repo, kQueue, "orders");

    for (int i = 0; i < 5; ++i) {
        repo.sendMessage("m-" + std::to_string(i), "ern:eqs:...:message:m", kQueue, "0123456789", {}, {}, MessagePriority::MEDIUM);
    }
    repo.flushQueueCounters();

    const auto received = repo.receiveMessages(kQueue, 3, 0);
    repo.flushQueueCounters();
    BOOST_TEST_REQUIRE(received.size() == 3U);

    // The bytes have not gone anywhere - a claimed message is still stored, and still the queue's.
    const auto queue = reread(repo, kQueue);
    BOOST_TEST(queue.available == 2);
    BOOST_TEST(queue.invisible == 3);
    BOOST_TEST(queue.size == 50);
}

BOOST_AUTO_TEST_CASE(ADeleteTakesTheMessageOutOfTheCounterThatHeldIt) {
    auto repo = freshRepository();
    queueOf(repo, kQueue, "orders");

    repo.sendMessage("m-1", "ern:eqs:...:message:m-1", kQueue, "0123456789", {}, {}, MessagePriority::MEDIUM);
    repo.sendMessage("m-2", "ern:eqs:...:message:m-2", kQueue, "0123456789", {}, {}, MessagePriority::MEDIUM);

    const auto received = repo.receiveMessages(kQueue, 1, 0);
    BOOST_TEST_REQUIRE(received.size() == 1U);
    repo.flushQueueCounters();

    // Deleting the claimed one has to come off "invisible". Taking it off "available" instead
    // would leave the queue reporting a message it no longer has as receivable, and this one as
    // still claimed - the shape of drift that is worst to debug, because both numbers look
    // plausible on their own.
    repo.deleteMessage(received.front().receiptHandle);
    repo.flushQueueCounters();

    const auto queue = reread(repo, kQueue);
    BOOST_TEST(queue.available == 1);
    BOOST_TEST(queue.invisible == 0);
    BOOST_TEST(queue.size == 10);
}

BOOST_AUTO_TEST_CASE(ADelayedMessageIsCountedApartFromAReceivableOne) {
    auto repo = freshRepository();

    // Its own ERN, not the one the other cases use: what a send does with a message is decided by
    // the queue configuration, which is cached per ERN for kQueueConfigTtl and outlives the
    // in-memory store between test cases. Reusing the ERN here reads the previous case's queue -
    // the one without a delay - and the test measures the cache rather than the counters.
    constexpr auto kDelayedQueue = "ern:eqs:eu-central-1:000000000000:production:queue:orders-delayed";

    Queue queue;
    queue.name = "orders-delayed";
    queue.ern = kDelayedQueue;
    queue.accountId = "000000000000";
    queue.nameSpace = "production";
    queue.region = "eu-central-1";
    queue.delay = 60;
    repo.upsertQueue(queue);

    repo.sendMessage("m-1", "ern:eqs:...:message:m-1", kDelayedQueue, "0123456789", {}, {}, MessagePriority::MEDIUM);
    repo.flushQueueCounters();

    const auto stored = reread(repo, kDelayedQueue);
    BOOST_TEST(stored.delayed == 1);
    BOOST_TEST(stored.available == 0);
    BOOST_TEST(stored.size == 10);
}

BOOST_AUTO_TEST_CASE(APurgeLeavesNothingBehindInTheCounters) {
    auto repo = freshRepository();
    queueOf(repo, kQueue, "orders");

    for (int i = 0; i < 4; ++i) {
        repo.sendMessage("m-" + std::to_string(i), "ern:eqs:...:message:m", kQueue, "0123456789", {}, {}, MessagePriority::MEDIUM);
    }

    // Deliberately without a flush first: the point is that an adjustment still pending when the
    // purge happens does not land afterwards and leave an emptied queue reporting messages.
    repo.purgeQueue(kQueue);
    repo.flushQueueCounters();

    const auto queue = reread(repo, kQueue);
    BOOST_TEST(queue.available == 0);
    BOOST_TEST(queue.invisible == 0);
    BOOST_TEST(queue.delayed == 0);
    BOOST_TEST(queue.size == 0);
}

BOOST_AUTO_TEST_CASE(NoCounterGoesBelowZero) {
    auto repo = freshRepository();
    queueOf(repo, kQueue, "orders");

    // With the arithmetic done in the database a negative can only mean something was counted
    // twice, which is a defect. Clamping where it is found keeps the next adjustment from
    // compounding it; recountQueues() is what puts the true number back.
    repo.adjustQueueCounters(kQueue, -5, -5, -5, -500);
    repo.flushQueueCounters();

    const auto queue = reread(repo, kQueue);
    BOOST_TEST(queue.available == 0);
    BOOST_TEST(queue.invisible == 0);
    BOOST_TEST(queue.delayed == 0);
    BOOST_TEST(queue.size == 0);
}

BOOST_AUTO_TEST_CASE(AnAdjustmentLeavesEveryOtherFieldAlone) {
    auto repo = freshRepository();
    auto queue = queueOf(repo, kQueue, "orders");

    queue.tags["owner"] = "fulfilment";
    queue.visibility = 45;
    repo.upsertQueue(queue);

    repo.adjustQueueCounters(kQueue, 1, 0, 0, 10);
    repo.flushQueueCounters();

    // The counters are one `$inc`, not a rewrite of the document from a copy read moments ago -
    // which would revert whatever somebody changed in between.
    const auto stored = reread(repo, kQueue);
    BOOST_TEST(stored.available == 1);
    BOOST_TEST(stored.visibility == 45);
    BOOST_TEST(stored.tags.at("owner") == "fulfilment");
}

BOOST_AUTO_TEST_CASE(TheRecountIsWhatCorrectsDriftRatherThanTheSourceOfTheNumbers) {
    auto repo = freshRepository();
    queueOf(repo, kQueue, "orders");

    repo.sendMessage("m-1", "ern:eqs:...:message:m-1", kQueue, "0123456789", {}, {}, MessagePriority::MEDIUM);
    repo.flushQueueCounters();

    // What the TTL index does: the message goes without anybody accounting for it. Nothing in the
    // application can see this happen, which is the whole reason the recount still exists.
    repo.clearMessages();
    repo.adjustQueueCounters(kQueue, 7, 0, 0, 700);
    repo.flushQueueCounters();
    BOOST_TEST(reread(repo, kQueue).available == 7);

    repo.recountQueues();
    const auto queue = reread(repo, kQueue);
    BOOST_TEST(queue.available == 0);
    BOOST_TEST(queue.size == 0);
}

BOOST_AUTO_TEST_CASE(ConcurrentAdjustmentsAllLand) {
    auto repo = freshRepository();
    queueOf(repo, kQueue, "orders");

    // Read-modify-write loses adjustments that overlap, and overlapping is the normal case for a
    // queue: every producer and every consumer moves these numbers. Sixteen threads is enough to
    // lose most of them if the arithmetic happens anywhere but in the database.
    constexpr int kThreads = 16;
    constexpr int kPerThread = 100;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&repo] {
            for (int i = 0; i < kPerThread; ++i) {
                repo.adjustQueueCounters(kQueue, 1, 0, 0, 10);
            }
        });
    }
    for (auto &thread: threads) thread.join();
    repo.flushQueueCounters();

    const auto queue = reread(repo, kQueue);
    BOOST_TEST(queue.available == kThreads * kPerThread);
    BOOST_TEST(queue.size == kThreads * kPerThread * 10);
}

BOOST_AUTO_TEST_CASE(AdjustingByNothingDoesNothing) {
    auto repo = freshRepository();
    const auto before = queueOf(repo, kQueue, "orders");

    repo.adjustQueueCounters(kQueue, 0, 0, 0, 0);
    repo.flushQueueCounters();

    const auto after = reread(repo, kQueue);
    BOOST_TEST(after.available == before.available);
    BOOST_TEST((after.modified == before.modified));
}
